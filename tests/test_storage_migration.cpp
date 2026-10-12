#include <gtest/gtest.h>
#include "chwell/schema/storage_migration.h"
#include "player_schema.h"
#include <stdexcept>

using namespace chwell::schema;
using chwell::storage::StorageDocument;
using chwell::storage::orm::Document;
namespace {
std::shared_ptr<const EntitySchema> definition(unsigned version) {
    auto fields = chwell::generated::SchemaPlayer::entity_schema()->fields();
    if (version == 2) {
        fields[1].name = "rank"; // Explicit rename_step of the same stable ID.
        FieldDefinition added;
        added.id = 100; added.name = "title"; added.stored = true;
        added.default_value = std::string("rookie");
        fields.push_back(added);
    }
    return std::make_shared<const EntitySchema>("SchemaPlayer", "players", 1, fields,
        std::vector<FieldId>{}, version);
}
StorageDocument original(bool legacy = false) {
    chwell::generated::SchemaPlayer player;
    EXPECT_TRUE(player.set_id("p1"));
    EXPECT_TRUE(player.set_level(7));
    auto doc = player.to_document();
    if (legacy) doc.erase(storage_version_field);
    return {"players:p1", doc.to_string(), 2000000000};
}
bool rename_step(const Document&, Document& destination, std::string*) {
    destination.set_string("rank", destination.get_string("level"));
    destination.erase("level");
    return true;
}
}

TEST(StorageMigration, LegacyAndStampedDryRunMaterializesDefaultsAndPreservesReceipt) {
    StorageMigration migration(definition(1), definition(2), rename_step);
    for (bool legacy : {true, false}) {
        const auto input = original(legacy);
        MigrationChange change;
        ASSERT_TRUE(migration.prepare(input, change));
        EXPECT_EQ(change.before.value, input.value);
        EXPECT_EQ(change.after.expire_at, input.expire_at);
        EXPECT_EQ(change.after.key, input.key);
        Document document;
        ASSERT_TRUE(document.from_string(change.after.value));
        EXPECT_EQ(document.get_string(storage_version_field), "2");
        EXPECT_EQ(document.get_string("rank"), "7");
        EXPECT_EQ(document.get_string("title"), "rookie");
        EXPECT_FALSE(document.has("level"));
        SchemaEntity entity(definition(2));
        ASSERT_TRUE(entity.load_document(document));
        EXPECT_EQ(std::get<std::int64_t>(entity.value(10)), 7);
    }
}

TEST(StorageMigration, VersionsPreventOldReadersAndImplicitUpgrade) {
    SchemaEntity next(definition(2));
    Document old;
    ASSERT_TRUE(old.from_string(original().value));
    EXPECT_FALSE(next.load_document(old));
    old.erase(storage_version_field);
    EXPECT_FALSE(next.load_document(old));
    StorageMigration migration(definition(1), definition(2), rename_step);
    MigrationChange change;
    ASSERT_TRUE(migration.prepare(original(), change));
    Document upgraded;
    ASSERT_TRUE(upgraded.from_string(change.after.value));
    chwell::generated::SchemaPlayer previous;
    EXPECT_FALSE(previous.load_document(upgraded));
    EXPECT_EQ(chwell::generated::SchemaPlayer::entity_schema()->version(), 1u);
    for (const auto& version : {"0", "01", "1x", "4294967296", "2"}) {
        old.set_string(storage_version_field, version);
        EXPECT_FALSE(previous.load_document(old));
    }
}

TEST(StorageMigration, StrictSourceRejectsUnknownMalformedAndWrongKeyWithoutChangingOut) {
    StorageMigration migration(definition(1), definition(2), rename_step);
    const auto input = original();
    MigrationChange out{input, input};
    for (const auto& field : {"level", "future"}) {
        Document doc; ASSERT_TRUE(doc.from_string(input.value));
        doc.set_string(field, "12junk");
        std::string error;
        EXPECT_FALSE(migration.prepare({input.key, doc.to_string()}, out, &error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(out.after.value, input.value);
    }
    EXPECT_FALSE(migration.prepare({"players:other", input.value}, out));
    EXPECT_FALSE(migration.prepare({input.key, input.value + "junk"}, out));
    EXPECT_FALSE(migration.prepare({input.key, "id=p1\nid=p2\n"}, out));
    EXPECT_EQ(out.before.expire_at, input.expire_at);
}

TEST(StorageMigration, TransformMustExplicitlyRemoveOldFieldsAndRetainIdentity) {
    for (int mode : {0, 1, 2, 3}) {
        StorageMigration migration(definition(1), definition(2), [=](const auto& source, auto& dest, auto* error) {
            if (mode == 0) return true; // Leaving level behind must fail, not silently lose it.
            rename_step(source, dest, error);
            if (mode == 1) dest.set_string("id", "other");
            if (mode == 2) dest.set_string("rank", "999");
            if (mode == 3) dest.set_string(storage_version_field, "2");
            return true;
        });
        MigrationChange out;
        EXPECT_FALSE(migration.prepare(original(), out));
        EXPECT_TRUE(out.after.value.empty());
    }
}

TEST(StorageMigration, CallbackFailureAndExceptionLeaveReceiptUntouched) {
    auto input = original();
    MigrationChange out{input, input};
    StorageMigration rejected(definition(1), definition(2), [](const auto&, auto&, auto*) { return false; });
    EXPECT_FALSE(rejected.prepare(input, out));
    StorageMigration throws(definition(1), definition(2), [](const auto&, auto&, auto*) -> bool {
        throw std::runtime_error("failed conversion");
    });
    EXPECT_THROW(throws.prepare(input, out), std::runtime_error);
    EXPECT_EQ(out.after.value, input.value);
}

TEST(StorageMigration, MissingOldValuesAreMaterializedBeforeTransform) {
    Document input; input.set_string("id", "p1");
    StorageMigration migration(definition(1), definition(2), rename_step);
    MigrationChange out;
    ASSERT_TRUE(migration.prepare({"players:p1", input.to_string()}, out));
    Document after; ASSERT_TRUE(after.from_string(out.after.value));
    EXPECT_EQ(after.get_string("rank"), "1");
}

TEST(StorageMigration, InvalidRegistrationAndReservedMetadataFailEarly) {
    EXPECT_THROW(StorageMigration(definition(1), definition(1), rename_step), std::invalid_argument);
    EXPECT_THROW(StorageMigration(definition(2), definition(1), rename_step), std::invalid_argument);
    EXPECT_THROW(StorageMigration(nullptr, definition(2), rename_step), std::invalid_argument);
    EXPECT_THROW(StorageMigration(definition(1), definition(2), {}), std::invalid_argument);
    auto fields = definition(1)->fields();
    fields[1].name = storage_version_field;
    EXPECT_THROW(EntitySchema("P", "players", 1, fields), std::invalid_argument);
}
