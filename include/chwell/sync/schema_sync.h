#pragma once

#include "chwell/schema/entity_schema.h"
#include <functional>

namespace chwell { namespace sync {

struct SchemaPacket {
    std::string entity_id;
    bool snapshot = false;
    std::vector<schema::FieldValue> fields;
};
struct SchemaSyncStats {
    std::uint64_t submitted_changes = 0;
    std::uint64_t packets = 0;
    std::uint64_t delivered_fields = 0;
};

// Owner-thread adapter; viewer identities must come from authenticated server
// state. Subscribe/unsubscribe can be connected to AOI enter/leave events.
// Entity keys and ownership are fixed until remove_entity. Load before add_entity.
class SchemaSyncRoom {
public:
    enum class Mode { Immediate, Tick };
    using Sender = std::function<void(const SchemaPacket&)>;
    explicit SchemaSyncRoom(Mode mode = Mode::Tick) : mode_(mode) {}
    bool add_entity(std::shared_ptr<schema::SchemaEntity> entity, std::string owner);
    bool remove_entity(const std::string& id);
    bool subscribe(const std::string& id, const std::string& viewer, Sender sender);
    void unsubscribe(const std::string& id, const std::string& viewer);
    bool set_value(const std::string& id, schema::FieldId field, schema::Value value, std::string* error = nullptr);
    // Flush direct generated setters too. At most one final value per field per viewer.
    void flush();
    const SchemaSyncStats& stats() const { return stats_; }
private:
    struct Subscriber { Sender sender; std::vector<schema::FieldValue> baseline; };
    struct Entry {
        std::shared_ptr<schema::SchemaEntity> entity;
        std::string owner;
        std::map<std::string, Subscriber> subscribers;
    };
    void publish(const std::string& id);
    Mode mode_;
    bool sending_ = false;
    std::map<std::string, Entry> entities_;
    SchemaSyncStats stats_;
};

} } // namespace chwell::sync
