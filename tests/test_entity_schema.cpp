#include <gtest/gtest.h>
#include "player_schema.h"
#include "chwell/sync/schema_sync.h"
#include "chwell/storage/memory_storage.h"
#include "chwell/storage/orm/repository.h"
#include <limits>

using chwell::generated::SchemaPlayer;
using chwell::schema::Value;
using chwell::sync::SchemaPacket;
using chwell::sync::SchemaSyncRoom;
using chwell::storage::orm::Document;
namespace {
bool has(const SchemaPacket& packet, chwell::schema::FieldId id) {
    for (const auto& field : packet.fields) if (field.id == id) return true;
    return false;
}
std::shared_ptr<SchemaPlayer> player(const std::string& id = "p1") {
    auto result = std::make_shared<SchemaPlayer>();
    EXPECT_TRUE(result->set_id(id));
    result->clear_dirty();
    return result;
}
}

TEST(EntitySchema, GeneratedDefaultsConstraintsAndImmutableIdentity) {
    SchemaPlayer p;
    EXPECT_EQ(p.get_level(), 1);
    EXPECT_FALSE(p.is_dirty());
    ASSERT_TRUE(p.set_id("p1"));
    EXPECT_FALSE(p.set_id("p2"));
    EXPECT_FALSE(p.set_level(101));
    EXPECT_FALSE(p.set_gold(-1));
    EXPECT_FALSE(p.set_secret(std::string(129, 'x')));
    EXPECT_FALSE(p.set_speed(std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(p.set_speed(std::numeric_limits<double>::infinity()));
    EXPECT_FALSE(p.set_value(10, std::string("wrong type")));
    EXPECT_FALSE(p.set_value(4, std::int64_t{1}));
    EXPECT_EQ(p.get_level(), 1);
    EXPECT_EQ(p.get_id(), "p1");
    p.clear_dirty();
    EXPECT_TRUE(p.set_speed(2.0));
    EXPECT_TRUE(p.set_online(true));
    EXPECT_FALSE(p.is_dirty());
    EXPECT_TRUE(p.set_level(2));
    EXPECT_TRUE(p.is_field_dirty("level"));
}

TEST(EntitySchema, MetadataRejectsReservedDuplicateAndInvalidDefaults) {
    auto fields = SchemaPlayer::entity_schema()->fields();
    using chwell::schema::EntitySchema;
    EXPECT_THROW(EntitySchema("P", "players", 1, fields, {10}), std::invalid_argument);
    fields.push_back(fields.back());
    EXPECT_THROW(EntitySchema("P", "players", 1, fields), std::invalid_argument);
    fields.pop_back();
    fields[1].default_value = std::int64_t{0};
    EXPECT_THROW(EntitySchema("P", "players", 1, fields), std::invalid_argument);
    EXPECT_THROW(EntitySchema("P", "players", 20, fields), std::invalid_argument);
    EXPECT_THROW(chwell::schema::SchemaEntity(nullptr), std::invalid_argument);
}

TEST(EntitySchema, RepositoryRoundTripStoresPrivateAndResetsTransient) {
    chwell::storage::MemoryStorage storage;
    chwell::storage::orm::Repository<SchemaPlayer> repo(&storage, "players");
    auto p = player();
    ASSERT_TRUE(p->set_gold(std::numeric_limits<std::int64_t>::max()));
    ASSERT_TRUE(p->set_secret("server-value"));
    ASSERT_TRUE(p->set_speed(2.5));
    ASSERT_TRUE(p->set_online(true));
    auto doc = p->to_document();
    EXPECT_TRUE(doc.has("secret"));
    EXPECT_FALSE(doc.has("speed"));
    EXPECT_FALSE(doc.has("online"));
    ASSERT_TRUE(repo.save(*p).ok);
    auto loaded = repo.find("p1");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->get_gold(), std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(loaded->get_secret(), "server-value");
    EXPECT_DOUBLE_EQ(loaded->get_speed(), 1.0);
    EXPECT_FALSE(loaded->get_online());
    EXPECT_FALSE(loaded->is_dirty());
}

TEST(EntitySchema, InvalidLoadIsAtomicAndMissingValuesUseDefaults) {
    auto p = player();
    ASSERT_TRUE(p->set_level(8));
    const auto original = p->to_document().to_string();
    for (const auto& malformed : {"12junk", "9223372036854775808", " 5", "0"}) {
        auto doc = p->to_document();
        doc.set_string("level", malformed);
        EXPECT_FALSE(p->load_document(doc));
        EXPECT_EQ(p->to_document().to_string(), original);
        EXPECT_TRUE(p->is_field_dirty("level"));
        EXPECT_THROW(p->from_document(doc), std::invalid_argument);
    }
    Document missing;
    EXPECT_FALSE(p->load_document(missing));
    missing.set_string("id", "p2");
    missing.set_string("future_field", "ignored");
    ASSERT_TRUE(p->load_document(missing));
    EXPECT_EQ(p->get_id(), "p2");
    EXPECT_EQ(p->get_level(), 1);
    EXPECT_FALSE(p->is_dirty());
}

TEST(EntitySchema, NumericStoragePreservesDoubleAndInt64Extremes) {
    auto fields = SchemaPlayer::entity_schema()->fields();
    fields[1].int_min.reset(); fields[1].int_max.reset();
    fields[4].stored = true;
    auto definition = std::make_shared<const chwell::schema::EntitySchema>("P", "players", 1, fields);
    chwell::schema::SchemaEntity p(definition), loaded(definition);
    ASSERT_TRUE(p.set_value(1, std::string("p1")));
    ASSERT_TRUE(p.set_value(10, std::numeric_limits<std::int64_t>::min()));
    ASSERT_TRUE(p.set_value(40, 0.1));
    ASSERT_TRUE(loaded.load_document(p.to_document()));
    EXPECT_EQ(std::get<std::int64_t>(loaded.value(10)), std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(std::get<double>(loaded.value(40)), 0.1);
    for (const auto& text : {"nan", "inf", "1.0x", " 1", "1e309"}) {
        auto doc = p.to_document(); doc.set_string("speed", text);
        EXPECT_FALSE(loaded.load_document(doc));
        EXPECT_EQ(std::get<double>(loaded.value(40)), 0.1);
    }
}

TEST(EntitySchema, GeneratedContentAndClientMetadata) {
    const auto rows = SchemaPlayer::content();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].id(), "template_warrior");
    EXPECT_EQ(rows[0].get_level(), 5);
    EXPECT_DOUBLE_EQ(rows[0].get_speed(), 1.25);
    EXPECT_FALSE(rows[0].is_dirty());
    const auto metadata = SchemaPlayer::client_schema();
    EXPECT_EQ(metadata.find("secret"), std::string::npos);
    EXPECT_EQ(metadata.find("server-default"), std::string::npos);
    EXPECT_NE(metadata.find("gold"), std::string::npos);
}

TEST(SchemaSync, OwnerPublicSnapshotTickCoalescingAndAoiLeave) {
    SchemaSyncRoom room;
    auto p = player();
    ASSERT_TRUE(room.add_entity(p, "owner"));
    std::vector<SchemaPacket> owner, observer, entrant;
    ASSERT_TRUE(room.subscribe("p1", "owner", [&](const auto& packet) { owner.push_back(packet); }));
    ASSERT_TRUE(room.subscribe("p1", "observer", [&](const auto& packet) { observer.push_back(packet); }));
    ASSERT_TRUE(owner[0].snapshot);
    EXPECT_TRUE(has(owner[0], 20)); EXPECT_FALSE(has(observer[0], 20));
    EXPECT_FALSE(has(owner[0], 30)); EXPECT_FALSE(has(observer[0], 30));
    ASSERT_TRUE(room.set_value("p1", 10, std::int64_t{2}));
    ASSERT_TRUE(room.set_value("p1", 10, std::int64_t{3}));
    ASSERT_TRUE(p->set_gold(9)); ASSERT_TRUE(p->set_secret("hidden"));
    ASSERT_TRUE(room.subscribe("p1", "entrant", [&](const auto& packet) { entrant.push_back(packet); }));
    room.flush();
    ASSERT_EQ(owner.size(), 2u); ASSERT_EQ(observer.size(), 2u); ASSERT_EQ(entrant.size(), 1u);
    ASSERT_EQ(observer.back().fields.size(), 1u);
    EXPECT_EQ(std::get<std::int64_t>(observer.back().fields[0].value), 3);
    EXPECT_TRUE(has(owner.back(), 20)); EXPECT_FALSE(has(owner.back(), 30));
    EXPECT_TRUE(p->is_field_dirty("level"));
    EXPECT_EQ(room.stats().submitted_changes, 2u);
    room.unsubscribe("p1", "observer");
    ASSERT_TRUE(p->set_level(4)); room.flush();
    EXPECT_EQ(observer.size(), 2u);
    ASSERT_TRUE(p->set_level(5)); ASSERT_TRUE(p->set_level(4));
    const auto count = owner.size(); room.flush(); EXPECT_EQ(owner.size(), count);
    EXPECT_TRUE(room.remove_entity("p1"));
    ASSERT_TRUE(p->set_level(6)); room.flush(); EXPECT_EQ(owner.size(), count);
}

TEST(SchemaSync, ImmediateAndDirectSettersHaveIndependentStorageDirtyState) {
    SchemaSyncRoom room(SchemaSyncRoom::Mode::Immediate);
    auto p = player(); ASSERT_TRUE(room.add_entity(p, "owner"));
    std::vector<SchemaPacket> packets;
    ASSERT_TRUE(room.subscribe("p1", "owner", [&](const auto& packet) { packets.push_back(packet); }));
    ASSERT_TRUE(room.set_value("p1", 10, std::int64_t{2}));
    EXPECT_EQ(packets.size(), 2u);
    ASSERT_TRUE(p->set_level(3)); p->clear_dirty(); room.flush();
    EXPECT_EQ(packets.size(), 3u);
    EXPECT_FALSE(p->is_dirty());
    EXPECT_FALSE(room.set_value("p1", 10, std::int64_t{101}));
    EXPECT_EQ(packets.size(), 3u);
}

TEST(SchemaSync, FailedViewerRetriesWithoutBlockingOthersAndRejectsReentrancy) {
    SchemaSyncRoom room;
    auto p = player(); ASSERT_TRUE(room.add_entity(p, "owner"));
    bool fail = false; int bad = 0, good = 0;
    ASSERT_TRUE(room.subscribe("p1", "a", [&](const auto&) { if (fail) throw std::runtime_error("offline"); ++bad; }));
    ASSERT_TRUE(room.subscribe("p1", "b", [&](const auto&) {
        ++good;
        EXPECT_FALSE(room.remove_entity("p1"));
        EXPECT_FALSE(room.set_value("p1", 10, std::int64_t{8}));
        EXPECT_THROW(room.flush(), std::logic_error);
    }));
    ASSERT_TRUE(p->set_level(2)); fail = true;
    EXPECT_THROW(room.flush(), std::runtime_error);
    EXPECT_EQ(bad, 1); EXPECT_EQ(good, 2);
    fail = false; room.flush();
    EXPECT_EQ(bad, 2); EXPECT_EQ(good, 2);
    EXPECT_TRUE(p->is_dirty());
}

TEST(SchemaSync, FailedInitialSubscriptionCanRetryAndAttachedKeyCannotReload) {
    SchemaSyncRoom room; auto p = player();
    EXPECT_FALSE(room.add_entity(std::make_shared<SchemaPlayer>(), "owner"));
    ASSERT_TRUE(room.add_entity(p, "owner"));
    EXPECT_THROW(room.subscribe("p1", "v", [](const auto&) { throw std::runtime_error("offline"); }), std::runtime_error);
    EXPECT_TRUE(room.subscribe("p1", "v", [](const auto&) {}));
    Document doc; doc.set_string("id", "p2"); ASSERT_TRUE(p->load_document(doc));
    EXPECT_THROW(room.flush(), std::logic_error);
    EXPECT_THROW(room.subscribe("p1", "v2", [](const auto&) {}), std::logic_error);
}
