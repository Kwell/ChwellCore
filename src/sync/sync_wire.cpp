#include "chwell/sync/sync_wire.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace chwell { namespace sync {
namespace {
bool fail(std::string* error, const char* message) { if (error) *error = message; return false; }
bool utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        int count;
        std::uint32_t value, minimum;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 31; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 15; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 7; minimum = 0x10000; }
        else return false;
        if (text.size() - i < static_cast<std::size_t>(count)) return false;
        while (count--) {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
bool identifier(const std::string& value) {
    return !value.empty() && value.size() <= 256 && value.find('\0') == std::string::npos && utf8(value);
}
bool structure(const SyncWirePacket& wire, std::string* error) {
    if (!identifier(wire.schema_name) || !identifier(wire.stream) || !identifier(wire.packet.entity_id) ||
        !wire.schema_version || !wire.sequence || wire.packet.fields.size() > 1024)
        return fail(error, "Invalid packet identity, version, sequence or field count");
    if (wire.packet.snapshot ? wire.base_sequence != 0 :
        (wire.base_sequence == 0 || wire.base_sequence == std::numeric_limits<std::uint64_t>::max() ||
         wire.sequence != wire.base_sequence + 1)) return fail(error, "Invalid snapshot/delta baseline");
    schema::FieldId previous = 0;
    for (const auto& field : wire.packet.fields) {
        if (field.id <= previous) return fail(error, "Field IDs must be unique and increasing");
        previous = field.id;
        if (const auto* text = std::get_if<std::string>(&field.value)) {
            if (text->size() > sync_wire_max_bytes || !utf8(*text)) return fail(error, "Invalid UTF-8 or string size");
        }
        if (const auto* number = std::get_if<double>(&field.value)) {
            if (!std::isfinite(*number)) return fail(error, "Non-finite double");
        }
    }
    return true;
}
bool validate(const SyncWirePacket& wire, const schema::EntitySchema& definition, bool owner, std::string* error) {
    if (!structure(wire, error)) return false;
    if (wire.schema_name != definition.name()) return fail(error, "Schema name mismatch");
    for (const auto& field : wire.packet.fields) {
        const auto* metadata = definition.find(field.id);
        if (!metadata || !schema::EntitySchema::visible(*metadata, owner)) return fail(error, "Unknown or forbidden field");
        if (!definition.validate(field.id, field.value, error)) return false;
        if (field.id == definition.key() && std::get<std::string>(field.value) != wire.packet.entity_id)
            return fail(error, "Entity key mismatch");
    }
    if (wire.packet.snapshot) {
        std::size_t required = 0;
        for (const auto& field : definition.fields()) if (schema::EntitySchema::visible(field, owner)) ++required;
        if (wire.packet.fields.size() != required) return fail(error, "Incomplete snapshot");
    }
    return true;
}
void integer(std::string& out, std::uint64_t value, int width) {
    for (int i = width - 1; i >= 0; --i) out.push_back(static_cast<char>((value >> (i * 8)) & 255));
}
void text(std::string& out, const std::string& value, int width) {
    integer(out, value.size(), width); out += value;
}
struct Reader {
    std::string_view bytes;
    std::size_t offset = 0;
    std::uint64_t integer(int width) {
        if (bytes.size() - offset < static_cast<std::size_t>(width)) throw std::invalid_argument("Truncated sync packet");
        std::uint64_t value = 0;
        for (int i = 0; i < width; ++i) value = (value << 8) | static_cast<unsigned char>(bytes[offset++]);
        return value;
    }
    std::string text(int width, std::size_t limit) {
        const auto length = integer(width);
        if (length > limit || length > bytes.size() - offset) throw std::invalid_argument("Invalid sync string length");
        std::string value(bytes.substr(offset, static_cast<std::size_t>(length)));
        offset += static_cast<std::size_t>(length); return value;
    }
};
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559, "Sync v1 requires IEEE 754 binary64");
}

bool encode_sync_wire(const SyncWirePacket& wire, const schema::EntitySchema& definition, bool owner,
                      std::string& bytes, std::string* error, std::size_t limit) {
    if (!validate(wire, definition, owner, error)) return false;
    const auto bound = std::min(limit, sync_wire_max_bytes);
    std::string pending = "CHWS";
    integer(pending, sync_wire_version, 2);
    integer(pending, wire.packet.snapshot ? 1 : 2, 1); integer(pending, 0, 1);
    integer(pending, wire.schema_version, 4);
    integer(pending, wire.sequence, 8); integer(pending, wire.base_sequence, 8);
    text(pending, wire.schema_name, 2); text(pending, wire.stream, 2); text(pending, wire.packet.entity_id, 2);
    integer(pending, wire.packet.fields.size(), 2);
    for (const auto& field : wire.packet.fields) {
        integer(pending, field.id, 4);
        if (const auto* int_value = std::get_if<std::int64_t>(&field.value)) {
            integer(pending, 1, 1); integer(pending, static_cast<std::uint64_t>(*int_value), 8);
        } else if (const auto* double_value = std::get_if<double>(&field.value)) {
            std::uint64_t bits; std::memcpy(&bits, double_value, 8);
            integer(pending, 2, 1); integer(pending, bits, 8);
        } else if (const auto* bool_value = std::get_if<bool>(&field.value)) {
            integer(pending, 3, 1); integer(pending, *bool_value ? 1 : 0, 1);
        } else { integer(pending, 4, 1); text(pending, std::get<std::string>(field.value), 4); }
        if (pending.size() > bound) return fail(error, "Sync packet exceeds byte limit");
    }
    if (pending.size() > bound) return fail(error, "Sync packet exceeds byte limit");
    bytes.swap(pending); if (error) error->clear(); return true;
}

bool decode_sync_wire(std::string_view bytes, SyncWirePacket& packet, std::string* error, std::size_t limit) {
    if (bytes.size() > std::min(limit, sync_wire_max_bytes)) return fail(error, "Sync packet exceeds byte limit");
    if (bytes.substr(0, 4) != "CHWS") return fail(error, "Invalid sync magic");
    try {
        Reader reader{bytes, 4};
        if (reader.integer(2) != sync_wire_version) return fail(error, "Unsupported sync protocol version");
        const auto kind = reader.integer(1);
        if ((kind != 1 && kind != 2) || reader.integer(1) != 0) return fail(error, "Invalid packet kind or reserved flags");
        SyncWirePacket pending;
        pending.packet.snapshot = kind == 1;
        pending.schema_version = static_cast<std::uint32_t>(reader.integer(4));
        pending.sequence = reader.integer(8); pending.base_sequence = reader.integer(8);
        pending.schema_name = reader.text(2, 256); pending.stream = reader.text(2, 256);
        pending.packet.entity_id = reader.text(2, 256);
        const auto count = reader.integer(2);
        if (count > 1024) return fail(error, "Too many fields");
        for (std::size_t i = 0; i < count; ++i) {
            const auto id = static_cast<schema::FieldId>(reader.integer(4));
            schema::Value value;
            switch (reader.integer(1)) {
            case 1: {
                const auto bits = reader.integer(8);
                // Defined arithmetic conversion, including INT64_MIN.
                value = bits <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ?
                    static_cast<std::int64_t>(bits) : -1 - static_cast<std::int64_t>(~bits);
                break;
            }
            case 2: {
                const auto bits = reader.integer(8); double number;
                std::memcpy(&number, &bits, 8); value = number; break;
            }
            case 3: {
                const auto boolean = reader.integer(1);
                if (boolean > 1) return fail(error, "Invalid bool");
                value = boolean == 1; break;
            }
            case 4: value = reader.text(4, sync_wire_max_bytes); break;
            default: return fail(error, "Unknown field type tag");
            }
            pending.packet.fields.push_back({id, std::move(value)});
        }
        if (reader.offset != bytes.size()) return fail(error, "Trailing sync bytes");
        if (!structure(pending, error)) return false;
        packet = std::move(pending); if (error) error->clear(); return true;
    } catch (const std::invalid_argument& exception) { if (error) *error = exception.what(); return false; }
}

SchemaReplica::SchemaReplica(std::shared_ptr<const schema::EntitySchema> definition, std::uint32_t version,
    std::string entity, bool owner) : definition_(std::move(definition)), version_(version),
    entity_(std::move(entity)), is_owner_(owner) {
    if (!definition_ || !version_ || !identifier(entity_)) throw std::invalid_argument("Invalid replica metadata");
}
void SchemaReplica::reset_stream(std::string stream) {
    if (!identifier(stream)) throw std::invalid_argument("Invalid authenticated sync stream");
    stream_ = std::move(stream); values_.clear(); sequence_ = 0; ready_ = false;
}
ReplicaStatus SchemaReplica::apply(const SyncWirePacket& wire, std::string* error) {
    if (!validate(wire, *definition_, is_owner_, error)) return ReplicaStatus::Invalid;
    if (wire.schema_version != version_ || wire.packet.entity_id != entity_) {
        fail(error, "Replica schema version or entity mismatch"); return ReplicaStatus::Invalid;
    }
    if (stream_.empty() || wire.stream != stream_) { fail(error, "Stream mismatch; establish a new authenticated snapshot"); return ReplicaStatus::NeedsSnapshot; }
    if (wire.sequence <= sequence_) { fail(error, "Duplicate or stale packet"); return ReplicaStatus::Stale; }
    if (!wire.packet.snapshot && (!ready_ || wire.base_sequence != sequence_)) {
        fail(error, "Delta baseline missing; request a snapshot"); return ReplicaStatus::NeedsSnapshot;
    }
    auto pending = wire.packet.snapshot ? std::map<schema::FieldId, schema::Value>{} : values_;
    for (const auto& field : wire.packet.fields) pending[field.id] = field.value;
    values_.swap(pending); sequence_ = wire.sequence; ready_ = true;
    if (error) error->clear(); return ReplicaStatus::Applied;
}

} }
