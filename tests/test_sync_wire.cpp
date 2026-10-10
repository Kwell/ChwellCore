#include <gtest/gtest.h>
#include "chwell/sync/sync_wire.h"
#include <fstream>
#include <limits>
#include <random>
#include <sstream>

using namespace chwell;
using namespace chwell::sync;
namespace {
std::shared_ptr<const schema::EntitySchema> definition() {
    using schema::FieldDefinition; using schema::FieldType; using schema::Visibility;
    std::vector<FieldDefinition> fields{
        {1, "id", FieldType::String, std::string(), true, Visibility::Public},
        {2, "integer", FieldType::Int64, std::int64_t{0}, false, Visibility::Owner},
        {3, "number", FieldType::Double, 0.0, false, Visibility::Public},
        {4, "boolean", FieldType::Bool, false, false, Visibility::Public},
        {5, "text", FieldType::String, std::string(), false, Visibility::Public},
        {6, "secret", FieldType::String, std::string("never-client"), true, Visibility::Server}};
    fields[4].max_bytes = 256;
    return std::make_shared<const schema::EntitySchema>("WireFixture", "fixtures", 1, fields);
}
SyncWirePacket snapshot() {
    return {"WireFixture", 1, "stream", 1, 0, {"entity", true,
        {{1, std::string("entity")}, {2, std::numeric_limits<std::int64_t>::min()},
         {3, 0.1}, {4, true}, {5, std::string("中\0x", 5)}}}};
}
std::string hex(const std::string& bytes) {
    const char* digits = "0123456789abcdef"; std::string out;
    for (unsigned char c : bytes) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}
std::string fixture(const char* name) {
    std::ifstream input(std::string(SYNC_WIRE_FIXTURE_DIR) + "/" + name);
    std::string value; input >> value;
    if (!input) throw std::runtime_error("Missing wire fixture");
    return value;
}
SyncWirePacket delta(std::uint64_t sequence = 2, std::uint64_t base = 1) {
    return {"WireFixture", 1, "stream", sequence, base, {"entity", false, {{2, std::numeric_limits<std::int64_t>::max()}}}};
}
}

TEST(SyncWire, GoldenBytesAndAllTypedValuesRoundTrip) {
    for (const auto& sample : {std::make_pair(snapshot(), "sync_v1_snapshot.hex"),
                                std::make_pair(delta(), "sync_v1_delta.hex")}) {
        std::string bytes, error;
        ASSERT_TRUE(encode_sync_wire(sample.first, *definition(), true, bytes, &error)) << error;
        EXPECT_EQ(hex(bytes), fixture(sample.second));
        SyncWirePacket decoded;
        ASSERT_TRUE(decode_sync_wire(bytes, decoded, &error)) << error;
        EXPECT_EQ(decoded.packet.fields, sample.first.packet.fields);
        EXPECT_EQ(decoded.schema_name, sample.first.schema_name);
        EXPECT_EQ(decoded.schema_version, 1u);
        EXPECT_EQ(decoded.stream, "stream");
        EXPECT_EQ(decoded.sequence, sample.first.sequence);
        EXPECT_EQ(decoded.base_sequence, sample.first.base_sequence);
        EXPECT_EQ(decoded.packet.snapshot, sample.first.packet.snapshot);
    }
}

TEST(SyncWire, RejectsEveryTruncatedPrefixUnknownVersionAndTrailingBytesAtomically) {
    std::string bytes; ASSERT_TRUE(encode_sync_wire(snapshot(), *definition(), true, bytes));
    auto retained = delta();
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        EXPECT_FALSE(decode_sync_wire(std::string_view(bytes).substr(0, length), retained));
        EXPECT_EQ(retained.packet.fields, delta().packet.fields);
    }
    auto changed = bytes; changed[5] = 2;
    EXPECT_FALSE(decode_sync_wire(changed, retained));
    changed = bytes; changed[7] = 1;
    EXPECT_FALSE(decode_sync_wire(changed, retained));
    changed = bytes; changed[6] = 3;
    EXPECT_FALSE(decode_sync_wire(changed, retained));
    EXPECT_FALSE(decode_sync_wire(bytes + "x", retained));
    EXPECT_FALSE(decode_sync_wire(bytes, retained, nullptr, bytes.size() - 1));
    std::string output = "retained";
    EXPECT_FALSE(encode_sync_wire(snapshot(), *definition(), true, output, nullptr, bytes.size() - 1));
    EXPECT_EQ(output, "retained");
}

TEST(SyncWire, RejectsDuplicateForbiddenFieldsTypeErrorsAndMalformedUtf8) {
    const auto original = snapshot();
    std::string output = "retained";
    for (int mutation = 0; mutation < 10; ++mutation) {
        auto wire = original;
        switch (mutation) {
        case 0: wire.packet.fields.push_back({6, std::string("never-client")}); break;
        case 1: wire.packet.fields[1].id = 1; break;
        case 2: wire.packet.fields.pop_back(); break;
        case 3: wire.packet.fields[0].value = std::string("other"); break;
        case 4: wire.packet.fields[3].value = std::int64_t{1}; break;
        case 5: wire.packet.fields[2].value = std::numeric_limits<double>::infinity(); break;
        case 6: wire.packet.fields[4].value = std::string("\xc0\x80", 2); break;
        case 7: wire.packet.fields[4].value = std::string("\xed\xa0\x80", 3); break;
        case 8: wire.packet.fields[4].value = std::string(257, 'x'); break;
        case 9: wire.schema_name = std::string("bad\0name", 8); break;
        }
        EXPECT_FALSE(encode_sync_wire(wire, *definition(), true, output));
        EXPECT_EQ(output, "retained");
    }
    EXPECT_FALSE(encode_sync_wire(original, *definition(), false, output));
    auto public_packet = original; public_packet.packet.fields.erase(public_packet.packet.fields.begin() + 1);
    ASSERT_TRUE(encode_sync_wire(public_packet, *definition(), false, output));
    EXPECT_EQ(output.find("never-client"), std::string::npos);
    auto malformed = output; malformed.back() = static_cast<char>(0xff);
    SyncWirePacket decoded;
    EXPECT_FALSE(decode_sync_wire(malformed, decoded));
}

TEST(SyncWire, ReplicaBaselinesStalePacketsGapsAndAuthenticatedStreamReset) {
    SchemaReplica replica(definition(), 1, "entity", true);
    replica.reset_stream("stream");
    EXPECT_EQ(replica.apply(delta()), ReplicaStatus::NeedsSnapshot);
    EXPECT_TRUE(replica.values().empty());
    ASSERT_EQ(replica.apply(snapshot()), ReplicaStatus::Applied);
    ASSERT_EQ(replica.apply(delta()), ReplicaStatus::Applied);
    EXPECT_EQ(std::get<std::int64_t>(replica.values().at(2)), std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(replica.apply(delta()), ReplicaStatus::Stale);
    EXPECT_EQ(replica.apply(snapshot()), ReplicaStatus::Stale);
    EXPECT_EQ(replica.apply(delta(4, 3)), ReplicaStatus::NeedsSnapshot);
    EXPECT_EQ(replica.sequence(), 2u);
    auto resync = snapshot(); resync.sequence = 5;
    ASSERT_EQ(replica.apply(resync), ReplicaStatus::Applied);
    EXPECT_EQ(std::get<std::int64_t>(replica.values().at(2)), std::numeric_limits<std::int64_t>::min());
    auto wrong = snapshot(); wrong.stream = "previous-stream"; wrong.sequence = 9;
    EXPECT_EQ(replica.apply(wrong), ReplicaStatus::NeedsSnapshot);
    EXPECT_EQ(replica.sequence(), 5u);
    replica.reset_stream("replacement");
    EXPECT_FALSE(replica.ready()); EXPECT_TRUE(replica.values().empty());
    EXPECT_EQ(replica.apply(delta()), ReplicaStatus::NeedsSnapshot);
    resync.stream = "replacement"; resync.sequence = 1;
    EXPECT_EQ(replica.apply(resync), ReplicaStatus::Applied);
    EXPECT_EQ(replica.apply(snapshot()), ReplicaStatus::NeedsSnapshot);
}

TEST(SyncWire, InvalidPacketsCannotPartiallyMutateReplicaOrLeakOwnerState) {
    SchemaReplica replica(definition(), 1, "entity", true); replica.reset_stream("stream");
    ASSERT_EQ(replica.apply(snapshot()), ReplicaStatus::Applied);
    const auto before = replica.values();
    for (int mutation = 0; mutation < 6; ++mutation) {
        auto wire = delta();
        switch (mutation) {
        case 0: wire.packet.fields.push_back({5, std::string(257, 'x')}); break;
        case 1: wire.schema_version = 2; break;
        case 2: wire.packet.entity_id = "other"; break;
        case 3: wire.packet.fields.push_back({6, std::string("never-client")}); break;
        case 4: wire.packet.fields.push_back({1, std::string("entity")}); break;
        case 5: wire.sequence = 7; break;
        }
        EXPECT_EQ(replica.apply(wire), ReplicaStatus::Invalid);
        EXPECT_EQ(replica.values(), before); EXPECT_EQ(replica.sequence(), 1u);
    }
    SchemaReplica observer(definition(), 1, "entity", false); observer.reset_stream("stream");
    EXPECT_EQ(observer.apply(snapshot()), ReplicaStatus::Invalid);
    auto public_packet = snapshot(); public_packet.packet.fields.erase(public_packet.packet.fields.begin() + 1);
    EXPECT_EQ(observer.apply(public_packet), ReplicaStatus::Applied);
    EXPECT_EQ(observer.values().count(2), 0u);
}

TEST(SyncWire, Uint64SequenceLimitsAndDecoderMutationCorpus) {
    auto wire = snapshot(); wire.sequence = std::numeric_limits<std::uint64_t>::max();
    std::string bytes; ASSERT_TRUE(encode_sync_wire(wire, *definition(), true, bytes));
    SyncWirePacket decoded; ASSERT_TRUE(decode_sync_wire(bytes, decoded)); EXPECT_EQ(decoded.sequence, wire.sequence);
    auto invalid = delta(); invalid.base_sequence = wire.sequence; invalid.sequence = 0;
    EXPECT_FALSE(encode_sync_wire(invalid, *definition(), true, bytes));
    ASSERT_TRUE(encode_sync_wire(snapshot(), *definition(), true, bytes));
    std::mt19937 random(1337);
    for (int i = 0; i < 1500; ++i) {
        auto mutated = bytes;
        mutated[random() % mutated.size()] = static_cast<char>(random() & 255);
        SyncWirePacket output = delta();
        if (decode_sync_wire(mutated, output)) {
            std::string roundtrip;
            // A structural decoder may accept values a schema disallows. For
            // schema-valid packets canonical encoding must preserve every byte.
            if (encode_sync_wire(output, *definition(), true, roundtrip)) EXPECT_EQ(roundtrip, mutated);
        } else EXPECT_EQ(output.packet.fields, delta().packet.fields);
    }
}
