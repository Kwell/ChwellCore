#include "player_schema.h"
#include "chwell/storage/memory_storage.h"
#include "chwell/storage/orm/repository.h"
#include "chwell/sync/schema_sync.h"
#include <iostream>

int main() {
    using chwell::generated::SchemaPlayer;
    auto templates = SchemaPlayer::content();
    if (templates.empty()) return 1;
    auto entity = std::make_shared<SchemaPlayer>(templates.front());
    chwell::storage::MemoryStorage storage;
    chwell::storage::orm::Repository<SchemaPlayer> repo(&storage, "players");
    if (!repo.save(*entity).ok) return 1;
    auto loaded = repo.find(entity->id());
    if (!loaded || loaded->get_secret() != entity->get_secret()) return 1;
    chwell::sync::SchemaSyncRoom room;
    if (!room.add_entity(entity, "authenticated-owner")) return 1;
    unsigned packets = 0;
    if (!room.subscribe(entity->id(), "observer", [&](const auto& packet) {
        ++packets;
        std::cout << (packet.snapshot ? "snapshot" : "delta") << ": " << packet.fields.size() << " fields\n";
        for (const auto& field : packet.fields) {
            if (field.id == SchemaPlayer::field_gold || field.id == SchemaPlayer::field_secret)
                throw std::logic_error("Private field escaped visibility filtering");
        }
    })) return 1;
    if (!entity->set_level(6) || !entity->set_level(7) || !entity->set_gold(200)) return 1;
    room.flush(); // One public level change, final value 7.
    if (!repo.save(*entity).ok) return 1;
    entity->clear_dirty(); // Only after successful persistence.
    return packets == 2 ? 0 : 1;
}
