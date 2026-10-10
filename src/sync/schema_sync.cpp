#include "chwell/sync/schema_sync.h"
#include <stdexcept>
#include <exception>

namespace chwell { namespace sync {
namespace {
struct SendingGuard {
    bool& active;
    explicit SendingGuard(bool& value) : active(value) { active = true; }
    ~SendingGuard() { active = false; }
};
}
bool SchemaSyncRoom::add_entity(std::shared_ptr<schema::SchemaEntity> entity, std::string owner) {
    if (sending_ || !entity || entity->id().empty()) return false;
    const auto id = entity->id();
    return entities_.emplace(id, Entry{std::move(entity), std::move(owner), {}}).second;
}
bool SchemaSyncRoom::remove_entity(const std::string& id) {
    return !sending_ && entities_.erase(id) > 0;
}
bool SchemaSyncRoom::subscribe(const std::string& id, const std::string& viewer, Sender sender) {
    auto it = entities_.find(id);
    if (sending_ || it == entities_.end() || viewer.empty() || !sender || it->second.subscribers.count(viewer)) return false;
    if (it->second.entity->id() != id) throw std::logic_error("Load entity before attaching it to SchemaSyncRoom");
    auto snapshot = it->second.entity->snapshot(viewer == it->second.owner);
    SendingGuard guard(sending_);
    // If delivery fails, no subscription is installed; the caller can retry.
    sender(SchemaPacket{id, true, snapshot});
    it->second.subscribers.emplace(viewer, Subscriber{std::move(sender), snapshot});
    ++stats_.packets;
    stats_.delivered_fields += snapshot.size();
    return true;
}
void SchemaSyncRoom::unsubscribe(const std::string& id, const std::string& viewer) {
    if (sending_) return;
    auto it = entities_.find(id);
    if (it != entities_.end()) it->second.subscribers.erase(viewer);
}
bool SchemaSyncRoom::set_value(const std::string& id, schema::FieldId field, schema::Value value, std::string* error) {
    auto it = entities_.find(id);
    if (sending_ || it == entities_.end()) {
        if (error) *error = "Unknown entity or reentrant mutation";
        return false;
    }
    auto& entity = *it->second.entity;
    if (!entity.definition().validate(field, value, error)) return false;
    const bool changed = entity.value(field) != value;
    if (!entity.set_value(field, std::move(value), error)) return false;
    if (changed) ++stats_.submitted_changes;
    if (changed && mode_ == Mode::Immediate) publish(id);
    return true;
}
void SchemaSyncRoom::publish(const std::string& id) {
    auto& entry = entities_.at(id);
    if (entry.entity->id() != id) throw std::logic_error("Load entity before attaching it to SchemaSyncRoom");
    SendingGuard guard(sending_);
    std::exception_ptr failure;
    for (auto& subscriber : entry.subscribers) {
        const auto current = entry.entity->snapshot(subscriber.first == entry.owner);
        SchemaPacket packet{id, false, {}};
        for (std::size_t i = 0; i < current.size(); ++i) {
            if (!(current[i] == subscriber.second.baseline.at(i))) packet.fields.push_back(current[i]);
        }
        if (packet.fields.empty()) continue;
        try {
            subscriber.second.sender(packet);
            subscriber.second.baseline = current;
            ++stats_.packets;
            stats_.delivered_fields += packet.fields.size();
        } catch (...) {
            // Keep this viewer's baseline for retry, but deliver to other viewers.
            if (!failure) failure = std::current_exception();
        }
    }
    if (failure) std::rethrow_exception(failure);
}
void SchemaSyncRoom::flush() {
    if (sending_) throw std::logic_error("Reentrant schema sync flush");
    std::exception_ptr failure;
    for (const auto& entry : entities_) {
        try { publish(entry.first); } catch (...) { if (!failure) failure = std::current_exception(); }
    }
    if (failure) std::rethrow_exception(failure);
}

} } // namespace chwell::sync
