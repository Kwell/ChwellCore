#include "item.h"
#include "player.h"
#include "chwell/service/app_host.h"
#include "chwell/storage/memory_storage.h"
#include "chwell/storage/orm/repository.h"
#include "chwell/sync/schema_sync.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop_signal(int) { stopping = 1; }
using Player = chwell::generated::ReferencePlayer;

// Gameplay and sync belong to the main update thread. Network callbacks only echo.
class ReferenceGame final : public chwell::service::Component {
public:
    std::string name() const override { return "ReferenceGame"; }
    bool Init() override {
        const auto players = Player::content();
        const auto items = chwell::generated::ReferenceItem::content();
        if (players.empty()) return false;
        player_ = std::make_shared<Player>(players.front());
        const auto item = std::find_if(items.begin(), items.end(), [&](const auto& value) {
            return value.id() == player_->get_weapon();
        });
        if (item == items.end()) return false;
        std::cout << "weapon=" << item->id() << " power=" << item->get_power() << '\n';
        chwell::storage::orm::Repository<Player> repo(&storage_, "players");
        if (!repo.save(*player_).ok) return false;
        const auto loaded = repo.find(player_->id());
        if (!loaded || loaded->get_secret() != player_->get_secret()) return false;
        if (!room_.add_entity(player_, "owner")) return false;
        return room_.subscribe(player_->id(), "owner", [&](const auto& packet) { receive(packet, true); }) &&
               room_.subscribe(player_->id(), "spectator", [&](const auto& packet) { receive(packet, false); });
    }
    bool Update(std::int64_t) override {
        if (updated_) return true;
        if (!player_->set_level(6) || !player_->set_level(7) || !player_->set_gold(200))
            throw std::logic_error("Reference mutation failed");
        room_.flush();
        chwell::storage::orm::Repository<Player> repo(&storage_, "players");
        if (!repo.save(*player_).ok) throw std::logic_error("Reference persistence failed");
        player_->clear_dirty();
        const auto loaded = repo.find(player_->id());
        if (!loaded || loaded->get_level() != 7 || loaded->get_gold() != 200)
            throw std::logic_error("Reference round trip failed");
        updated_ = true;
        return true;
    }
    bool Shut() override {
        room_.remove_entity(player_ ? player_->id() : "");
        stopped_ = true;
        return true;
    }
    void on_message(const chwell::net::TcpConnectionPtr& connection, std::string_view data) override {
        connection->send(data);
    }
    bool smoke_passed() const { return updated_ && stopped_ && packets_ == 4; }
private:
    void receive(const chwell::sync::SchemaPacket& packet, bool owner) {
        for (const auto& field : packet.fields) {
            if (field.id == Player::field_secret || (!owner && field.id == Player::field_gold))
                throw std::logic_error("Private field escaped visibility filtering");
            if (!packet.snapshot && field.id == Player::field_level && std::get<std::int64_t>(field.value) != 7)
                throw std::logic_error("Tick did not coalesce level changes");
        }
        const std::size_t expected = packet.snapshot ? (owner ? 4 : 3) : (owner ? 2 : 1);
        if (packet.fields.size() != expected) throw std::logic_error("Unexpected visible fields");
        ++packets_;
        std::cout << (owner ? "owner " : "spectator ") << (packet.snapshot ? "snapshot " : "delta ")
                  << packet.fields.size() << " fields\n";
    }
    chwell::storage::MemoryStorage storage_;
    std::shared_ptr<Player> player_;
    chwell::sync::SchemaSyncRoom room_;
    unsigned packets_ = 0;
    bool updated_ = false;
    bool stopped_ = false;
};
} // namespace

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && std::string(argv[1]) == "--smoke";
    if (!smoke && !(argc == 3 && std::string(argv[1]) == "--serve")) {
        std::cerr << "Usage: reference_service --smoke | --serve service.conf\n";
        return 2;
    }
    try {
        chwell::core::Config config;
        if (!smoke && !config.load_from_file(argv[2])) return 1;
        chwell::service::AppHost host;
        ReferenceGame* game = nullptr; // Owned by AppHost; valid until host destruction.
        if (!host.register_component_factory("ReferenceGame", [&](const auto&) {
            auto component = std::make_unique<ReferenceGame>();
            game = component.get();
            return component;
        })) return 1;
        bool configured;
        if (smoke) {
            chwell::service::AppManifest manifest;
            manifest.listen_port = 0;
            manifest.worker_threads = 2;
            chwell::core::ComponentConfig entry;
            entry.name = "ReferenceGame";
            manifest.components.push_back(entry);
            configured = host.configure(manifest);
        } else {
            configured = host.configure(config);
        }
        if (!configured || !host.start()) {
            std::cerr << host.last_error() << '\n';
            return 1;
        }
        if (!game) { host.stop(); return 1; }
        std::signal(SIGINT, stop_signal);
        std::signal(SIGTERM, stop_signal);
        do {
            host.update();
            if (!smoke) std::this_thread::sleep_for(std::chrono::milliseconds(16));
        } while (!smoke && !stopping);
        host.stop();
        if (!game->smoke_passed()) return 1;
        std::cout << "PASS: content, ORM, owner/public sync and shutdown\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
