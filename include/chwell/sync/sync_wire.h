#pragma once

#include "chwell/sync/schema_sync.h"
#include <string_view>

namespace chwell { namespace sync {

// Stable v1 binary payload. Transport framing and authentication are external.
// sequence is per viewer/entity/stream, not a durable database revision.
struct SyncWirePacket {
    std::string schema_name;
    std::uint32_t schema_version = 0;
    std::string stream;
    std::uint64_t sequence = 0, base_sequence = 0;
    SchemaPacket packet;
};

constexpr std::uint16_t sync_wire_version = 1;
constexpr std::size_t sync_wire_max_bytes = 65535;
// Encoding validates types, constraints, visibility and snapshot completeness.
// Fields must be strictly increasing by ID. UTF-8 strings preserve embedded NUL.
// Output arguments are unchanged on failure; error contains diagnostics.
bool encode_sync_wire(const SyncWirePacket& packet, const schema::EntitySchema& definition,
                      bool is_owner, std::string& bytes, std::string* error = nullptr,
                      std::size_t max_bytes = sync_wire_max_bytes);
bool decode_sync_wire(std::string_view bytes, SyncWirePacket& packet,
                      std::string* error = nullptr, std::size_t max_bytes = sync_wire_max_bytes);

enum class ReplicaStatus { Applied, Stale, NeedsSnapshot, Invalid };
// Owner-thread client replica for ONE schema/entity/viewer. reset_stream is only
// called after authenticated stream establishment (e.g. a successful login),
// never just because an arbitrary packet carries a different stream ID.
class SchemaReplica {
public:
    SchemaReplica(std::shared_ptr<const schema::EntitySchema> definition, std::uint32_t version,
                  std::string entity, bool is_owner);
    void reset_stream(std::string stream); // Clears old values and baseline.
    ReplicaStatus apply(const SyncWirePacket& packet, std::string* error = nullptr);
    const std::map<schema::FieldId, schema::Value>& values() const { return values_; }
    std::uint64_t sequence() const { return sequence_; }
    bool ready() const { return ready_; }
private:
    std::shared_ptr<const schema::EntitySchema> definition_;
    std::uint32_t version_;
    std::string entity_, stream_;
    bool is_owner_, ready_ = false;
    std::uint64_t sequence_ = 0;
    std::map<schema::FieldId, schema::Value> values_;
};

} }
