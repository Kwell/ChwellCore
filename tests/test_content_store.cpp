#include "chwell/schema/content_store.h"
#include "player_schema.h"
#include "edge_schema.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <limits>
#include <thread>

namespace {
using Player = chwell::generated::SchemaPlayer;
using Edge = chwell::generated::std;
using namespace chwell::schema;
void number(std::string& out, std::uint64_t value, unsigned width) {
    while (width--) out.push_back(static_cast<char>((value >> (width * 8)) & 255));
}
void text(std::string& out, const std::string& value) { number(out, value.size(), 4); out += value; }
void value(std::string& out, const Value& input) {
    if (auto iv = std::get_if<std::int64_t>(&input)) { number(out, 1, 1); number(out, static_cast<std::uint64_t>(*iv), 8); }
    else if (auto dv = std::get_if<double>(&input)) {
        std::uint64_t bits; std::memcpy(&bits, dv, 8); number(out, 2, 1); number(out, bits, 8);
    } else if (auto bv = std::get_if<bool>(&input)) { number(out, 3, 1); number(out, *bv ? 1 : 0, 1); }
    else { number(out, 4, 1); text(out, std::get<std::string>(input)); }
}
using Row = std::map<FieldId, Value>;
Row row(const EntitySchema& schema, const std::string& id) {
    Row result;
    for (const auto& f : schema.fields()) result[f.id] = f.default_value;
    result[schema.key()] = id;
    return result;
}
struct Table { ContentTableDefinition definition; std::vector<Row> rows; };
std::string package(std::uint64_t version, std::vector<Table> tables) {
    std::string out = "CHCT";
    number(out, 1, 2); number(out, 0, 2); number(out, version, 8); number(out, tables.size(), 4);
    for (const auto& table : tables) {
        text(out, table.definition.schema->table()); text(out, table.definition.contract);
        number(out, table.rows.size(), 4);
        for (const auto& r : table.rows) {
            number(out, r.size(), 4);
            for (const auto& field : r) { number(out, field.first, 4); value(out, field.second); }
        }
    }
    return out;
}
std::string players(std::uint64_t version, std::int64_t level = 1) {
    auto r = row(*Player::entity_schema(), "hero"); r[Player::field_level] = level;
    return package(version, {{Player::content_definition(), {r}}});
}
}

TEST(ContentStore, WholeVersionPinnedViewsAndSingleRollback) {
    ContentStore store({Player::content_definition()});
    EXPECT_FALSE(store.snapshot());
    std::string error;
    ASSERT_TRUE(store.publish(players(7, 2), 0, &error)) << error;
    auto old = store.snapshot();
    ASSERT_TRUE(store.publish(players(8, 3), 1, &error)) << error;
    auto newer = store.snapshot();
    EXPECT_EQ(old->version(), 7u); EXPECT_EQ(old->generation(), 1u);
    EXPECT_EQ(Player::from_content(*old->find("players", "hero")).get_level(), 2);
    EXPECT_EQ(Player::from_content(*newer->find("players", "hero")).get_level(), 3);
    EXPECT_FALSE(newer->find("absent", "hero")); EXPECT_FALSE(newer->find("players", "absent"));
    EXPECT_EQ(newer->rows("players").size(), 1u);
    EXPECT_FALSE(store.rollback(1, &error)); EXPECT_NE(error.find("conflict"), std::string::npos);
    ASSERT_TRUE(store.rollback(2, &error));
    auto restored = store.snapshot();
    EXPECT_EQ(restored->version(), 7u); EXPECT_EQ(restored->generation(), 3u);
    EXPECT_EQ(Player::from_content(*restored->find("players", "hero")).get_level(), 2);
    EXPECT_FALSE(store.rollback(3, &error));
    EXPECT_FALSE(store.publish(players(8), 3, &error)); // high-water mark survives rollback
    EXPECT_EQ(store.snapshot(), restored);
    EXPECT_FALSE(store.publish(players(9), 2, &error));
    ASSERT_TRUE(store.publish(players(9, 4), 3, &error));
    EXPECT_EQ(newer->version(), 8u); // retained handles survive rollback/publication
    EXPECT_TRUE(error.empty());
}

TEST(ContentStore, PythonGeneratedPackageMatchesTypedDefaultsAndExactInt64) {
    ContentStore store({Player::content_definition(), Edge::content_definition()});
    std::string error;
    ASSERT_TRUE(store.publish_file(CONTENT_BUNDLE_FILE, 0, &error)) << error;
    const auto snapshot = store.snapshot();
    EXPECT_EQ(snapshot->version(), 1234u);
    for (const auto& expected : Player::content()) {
        auto actual = Player::from_content(*snapshot->find("players", expected.id()));
        for (const auto& field : Player::entity_schema()->fields()) EXPECT_EQ(actual.value(field.id), expected.value(field.id));
    }
    auto minimum = Edge::from_content(*snapshot->find("edges", "edge_min"));
    auto maximum = Edge::from_content(*snapshot->find("edges", "edge_max"));
    EXPECT_EQ(minimum.get_count(), std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(maximum.get_count(), std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(minimum.get_value(), Edge().get_value()); // Unicode and NUL defaults
    EXPECT_THROW(Player::from_content(*snapshot->find("edges", "edge_min")), std::invalid_argument);
}

TEST(ContentStore, EveryPrefixAndMalformedPackagesLeaveActiveAndPreviousUntouched) {
    ContentStore store({Player::content_definition()});
    ASSERT_TRUE(store.publish(players(1), 0));
    ASSERT_TRUE(store.publish(players(2), 1));
    auto current = store.snapshot();
    const auto valid = players(3);
    std::string error;
    for (std::size_t size = 0; size < valid.size(); ++size) {
        EXPECT_FALSE(store.publish(std::string_view(valid).substr(0, size), 2, &error)) << size;
        ASSERT_EQ(store.snapshot(), current);
    }
    std::vector<std::string> invalid = {valid + "extra", players(0),
        std::string(content_bundle_max_bytes + 1, 'x'), package(3, {}), players(3, 101)};
    auto changed = valid; changed[5] = 2; invalid.push_back(changed);
    changed = valid; changed[7] = 1; invalid.push_back(changed);
    auto definition = Player::content_definition(); definition.contract += "wrong";
    invalid.push_back(package(3, {{definition, {row(*definition.schema, "hero")}}}));
    auto r = row(*Player::entity_schema(), "hero");
    invalid.push_back(package(3, {{Player::content_definition(), {r, r}}}));
    r[Player::field_id] = std::string(); invalid.push_back(package(3, {{Player::content_definition(), {r}}}));
    r[Player::field_id] = std::string("\xc0\xaf", 2); invalid.push_back(package(3, {{Player::content_definition(), {r}}}));
    r = row(*Player::entity_schema(), "hero"); r.erase(Player::field_online);
    invalid.push_back(package(3, {{Player::content_definition(), {r}}}));
    r = row(*Player::entity_schema(), "hero"); r[Player::field_level] = true;
    invalid.push_back(package(3, {{Player::content_definition(), {r}}}));
    r = row(*Player::entity_schema(), "hero"); r[Player::field_speed] = std::numeric_limits<double>::infinity();
    invalid.push_back(package(3, {{Player::content_definition(), {r}}}));
    changed = valid; changed.back() = 2; invalid.push_back(changed); // bool=2
    changed = valid; changed[changed.size() - 2] = 99; invalid.push_back(changed); // unknown tag
    for (const auto& bytes : invalid) {
        EXPECT_FALSE(store.publish(bytes, 2, &error)); EXPECT_FALSE(error.empty());
        EXPECT_EQ(store.snapshot(), current);
    }
    EXPECT_FALSE(store.publish_file("nonexistent-content-package.chct", 2, &error));
    EXPECT_EQ(store.snapshot(), current);
    ASSERT_TRUE(store.rollback(2)); // bad candidates did not replace predecessor
    EXPECT_EQ(store.snapshot()->version(), 1u);
}

TEST(ContentStore, AllTablesReferencesAndCycleValidation) {
    auto player = Player::content_definition();
    player.references.push_back({Player::field_secret, "edges", false});
    auto edge = Edge::content_definition();
    edge.references.push_back({Edge::field_value, "players", false});
    ContentStore store({player, edge});
    auto p = row(*player.schema, "hero"); p[Player::field_secret] = std::string("sword");
    auto e = row(*edge.schema, "sword"); e[Edge::field_value] = std::string("hero");
    std::string error;
    ASSERT_TRUE(store.publish(package(1, {{edge, {e}}, {player, {p}}}), 0, &error)) << error;
    auto held = store.snapshot();
    auto broken = e; broken[Edge::field_value] = std::string("missing");
    EXPECT_FALSE(store.publish(package(2, {{edge, {broken}}, {player, {p}}}), 1, &error));
    EXPECT_NE(error.find("unresolved reference"), std::string::npos); EXPECT_EQ(store.snapshot(), held);
    p[Player::field_secret] = std::string("staff"); e[Edge::field_id] = std::string("staff");
    ASSERT_TRUE(store.publish(package(2, {{edge, {e}}, {player, {p}}}), 1, &error)) << error;
    EXPECT_TRUE(held->find("edges", "sword")); EXPECT_FALSE(held->find("edges", "staff"));
    EXPECT_TRUE(store.snapshot()->find("edges", "staff"));
    EXPECT_FALSE(store.publish(package(3, {{player, {p}}, {edge, {e}}}), 2)); // tables out of order
    auto empty_allowed = player; empty_allowed.references[0].allow_empty = true;
    ContentStore optional({empty_allowed, edge}); p[Player::field_secret] = std::string();
    EXPECT_TRUE(optional.publish(package(1, {{edge, {}}, {empty_allowed, {p}}}), 0));
    EXPECT_THROW(ContentStore({player}), std::invalid_argument); // missing compiled reference target
    EXPECT_THROW(ContentStore({edge, edge}), std::invalid_argument);
}

TEST(ContentStore, NumericExtremesUnicodeAndPrivateContentAreLossless) {
    ContentStore store({Edge::content_definition(), Player::content_definition()});
    auto edge = row(*Edge::entity_schema(), "sword");
    edge[Edge::field_count] = std::numeric_limits<std::int64_t>::min();
    auto player = row(*Player::entity_schema(), "hero");
    player[Player::field_gold] = std::numeric_limits<std::int64_t>::max();
    player[Player::field_secret] = std::string("private\0中文", 14);
    player[Player::field_speed] = 1.2345678901234567;
    ASSERT_TRUE(store.publish(package(1, {{Edge::content_definition(), {edge}}, {Player::content_definition(), {player}}}), 0));
    auto snapshot = store.snapshot();
    auto typed = Player::from_content(*snapshot->find("players", "hero"));
    EXPECT_EQ(typed.get_gold(), std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(typed.get_secret(), std::get<std::string>(player[Player::field_secret]));
    EXPECT_DOUBLE_EQ(typed.get_speed(), 1.2345678901234567);
    EXPECT_EQ(Edge::from_content(*snapshot->find("edges", "sword")).get_count(), std::numeric_limits<std::int64_t>::min());
}

TEST(ContentStore, ConcurrentReadersSeeCompleteVersionsAndStaleWritersFail) {
    ContentStore store({Player::content_definition(), Edge::content_definition()});
    auto versions = [](std::uint64_t v) {
        auto p = row(*Player::entity_schema(), "hero"); p[Player::field_level] = static_cast<std::int64_t>(v);
        auto e = row(*Edge::entity_schema(), "sword"); e[Edge::field_count] = static_cast<std::int64_t>(v);
        return package(v, {{Edge::content_definition(), {e}}, {Player::content_definition(), {p}}});
    };
    ASSERT_TRUE(store.publish(versions(1), 0));
    std::atomic<bool> done{false}, valid{true};
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 4; ++i) readers.emplace_back([&] {
        while (!done.load()) {
            auto snapshot = store.snapshot();
            if (std::get<std::int64_t>(snapshot->find("players", "hero")->value(Player::field_level)) !=
                static_cast<std::int64_t>(snapshot->version()) ||
                std::get<std::int64_t>(snapshot->find("edges", "sword")->value(Edge::field_count)) !=
                static_cast<std::int64_t>(snapshot->version())) valid = false;
        }
    });
    for (std::uint64_t v = 2; v <= 60; ++v) {
        if (!store.publish(versions(v), v - 1)) valid = false;
    }
    done = true;
    for (auto& reader : readers) reader.join();
    EXPECT_TRUE(valid.load());
    const auto generation = store.snapshot()->generation();
    std::atomic<unsigned> successes{0};
    std::thread a([&] { if (store.publish(versions(61), generation)) ++successes; });
    std::thread b([&] { if (store.publish(versions(62), generation)) ++successes; });
    a.join(); b.join(); EXPECT_EQ(successes.load(), 1u);
    EXPECT_EQ(store.snapshot()->generation(), generation + 1);
}
