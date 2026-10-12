#include "chwell/schema/content_store.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace chwell { namespace schema {
namespace {
bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}
bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        auto byte = static_cast<unsigned char>(text[i++]);
        if (byte < 128) continue;
        unsigned following;
        std::uint32_t code, minimum;
        if (byte >= 0xc2 && byte <= 0xdf) { following = 1; code = byte & 31; minimum = 0x80; }
        else if (byte >= 0xe0 && byte <= 0xef) { following = 2; code = byte & 15; minimum = 0x800; }
        else if (byte >= 0xf0 && byte <= 0xf4) { following = 3; code = byte & 7; minimum = 0x10000; }
        else return false;
        if (text.size() - i < following) return false;
        while (following--) {
            byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0) != 0x80) return false;
            code = (code << 6) | (byte & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
struct Reader {
    std::string_view bytes;
    std::size_t offset = 0;
    std::uint64_t integer(unsigned width) {
        if (bytes.size() - offset < width) throw std::invalid_argument("Truncated content package");
        std::uint64_t value = 0;
        while (width--) value = (value << 8) | static_cast<unsigned char>(bytes[offset++]);
        return value;
    }
    std::string text() {
        auto size = integer(4);
        if (size > content_bundle_max_string || size > bytes.size() - offset)
            throw std::invalid_argument("Invalid content string length");
        std::string result(bytes.substr(offset, static_cast<std::size_t>(size)));
        offset += static_cast<std::size_t>(size);
        if (!valid_utf8(result)) throw std::invalid_argument("Invalid content UTF-8");
        return result;
    }
    Value value() {
        switch (integer(1)) {
        case 1: {
            auto bits = integer(8);
            // Defined conversion including INT64_MIN, without unsigned-to-signed overflow.
            return bits <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                ? static_cast<std::int64_t>(bits) : -1 - static_cast<std::int64_t>(~bits);
        }
        case 2: {
            auto bits = integer(8);
            double result;
            std::memcpy(&result, &bits, 8);
            if (!std::isfinite(result)) throw std::invalid_argument("Non-finite content double");
            return result;
        }
        case 3: {
            auto flag = integer(1);
            if (flag > 1) throw std::invalid_argument("Invalid content boolean");
            return flag == 1;
        }
        case 4: return text();
        default: throw std::invalid_argument("Unknown content value tag");
        }
    }
};
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
              "Content v1 requires IEEE 754 binary64");

std::map<std::string, ContentTableDefinition> definitions(std::vector<ContentTableDefinition> input) {
    if (input.empty() || input.size() > content_bundle_max_tables)
        throw std::invalid_argument("Invalid content definition count");
    std::map<std::string, ContentTableDefinition> result;
    for (auto& table : input) {
        if (!table.schema || table.contract.empty() || table.contract.size() > content_bundle_max_string ||
            !valid_utf8(table.contract) || table.schema->table().size() > content_bundle_max_string ||
            !valid_utf8(table.schema->table()) || table.schema->fields().size() > content_bundle_max_fields)
            throw std::invalid_argument("Invalid compiled content definition");
        const auto name = table.schema->table();
        if (!result.emplace(name, std::move(table)).second) throw std::invalid_argument("Duplicate content definition");
    }
    for (const auto& table : result) {
        std::unordered_set<FieldId> used;
        for (const auto& ref : table.second.references) {
            const auto* field = table.second.schema->find(ref.field);
            if (!field || field->type != FieldType::String || field->id == table.second.schema->key() ||
                !used.insert(ref.field).second || !result.count(ref.table))
                throw std::invalid_argument("Invalid compiled content reference");
        }
    }
    return result;
}
} // namespace

const ContentRow* ContentSnapshot::find(const std::string& table, const std::string& id) const {
    auto t = tables_->find(table);
    if (t == tables_->end()) return nullptr;
    auto row = t->second.find(id);
    return row == t->second.end() ? nullptr : &row->second;
}
const std::map<std::string, ContentRow>& ContentSnapshot::rows(const std::string& table) const {
    return tables_->at(table);
}
ContentStore::ContentStore(std::vector<ContentTableDefinition> input) : definitions_(definitions(std::move(input))) {}
std::shared_ptr<const ContentSnapshot> ContentStore::snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return current_;
}
bool ContentStore::publish(std::string_view bytes, std::uint64_t expected_generation, std::string* error) {
    std::string where = "content";
    try {
        if (bytes.size() > content_bundle_max_bytes || bytes.substr(0, 4) != "CHCT")
            return fail(error, "Invalid content size or magic");
        Reader reader{bytes, 4};
        if (reader.integer(2) != 1 || reader.integer(2) != 0) return fail(error, "Unsupported content format or flags");
        auto pending = std::shared_ptr<ContentSnapshot>(new ContentSnapshot());
        pending->version_ = reader.integer(8);
        if (!pending->version_) return fail(error, "Content version must be nonzero");
        auto count = reader.integer(4);
        if (count != definitions_.size()) return fail(error, "Content table set mismatch");
        auto tables = std::make_shared<ContentSnapshot::Tables>();
        std::size_t total_rows = 0;
        std::string last_table;
        for (std::uint64_t i = 0; i < count; ++i) {
            auto name = reader.text();
            where = "table " + name;
            auto definition = definitions_.find(name);
            if (name <= last_table || definition == definitions_.end()) return fail(error, where + ": unknown or unordered table");
            last_table = name;
            if (reader.text() != definition->second.contract) return fail(error, where + ": compiled schema contract mismatch");
            const auto& schema = *definition->second.schema;
            auto row_count = reader.integer(4);
            if (row_count > content_bundle_max_rows - total_rows) return fail(error, "Content row limit exceeded");
            total_rows += static_cast<std::size_t>(row_count);
            auto& rows = (*tables)[name];
            for (std::uint64_t r = 0; r < row_count; ++r) {
                where = "table " + name + " row " + std::to_string(r + 1);
                if (reader.integer(4) != schema.fields().size()) return fail(error, where + ": incomplete field set");
                ContentRow row;
                row.schema_ = definition->second.schema;
                for (const auto& field : schema.fields()) {
                    if (reader.integer(4) != field.id) return fail(error, where + ": unknown or unordered field ID");
                    auto value = reader.value();
                    std::string diagnostic;
                    if (!schema.validate(field.id, value, &diagnostic)) return fail(error, where + ": " + diagnostic);
                    row.values_.emplace(field.id, std::move(value));
                }
                auto id = std::get<std::string>(row.value(schema.key()));
                if (id.empty() || !rows.emplace(id, std::move(row)).second) return fail(error, where + ": empty or duplicate ID");
            }
        }
        if (reader.offset != bytes.size()) return fail(error, "Trailing content bytes");
        // All tables are available before references are checked, including cycles.
        for (const auto& definition : definitions_) {
            for (const auto& row : tables->at(definition.first)) {
                for (const auto& ref : definition.second.references) {
                    const auto& id = std::get<std::string>(row.second.value(ref.field));
                    if (id.empty() && ref.allow_empty) continue;
                    if (!tables->at(ref.table).count(id))
                        return fail(error, "table " + definition.first + " ID " + row.first + " field " +
                                    std::to_string(ref.field) + ": unresolved reference " + ref.table + ".id");
                }
            }
        }
        pending->tables_ = std::move(tables);
        std::lock_guard<std::mutex> guard(mutex_);
        const auto generation = current_ ? current_->generation() : 0;
        if (generation != expected_generation) return fail(error, "Content generation conflict");
        if (pending->version_ <= highest_version_) return fail(error, "Content version must exceed previously published versions");
        if (generation == std::numeric_limits<std::uint64_t>::max()) return fail(error, "Content generation exhausted");
        pending->generation_ = generation + 1;
        previous_ = current_;
        highest_version_ = pending->version_;
        current_ = std::move(pending);
        if (error) error->clear();
        return true;
    } catch (const std::invalid_argument& exc) { return fail(error, where + ": " + exc.what()); }
}
bool ContentStore::publish_file(const std::string& path, std::uint64_t expected_generation, std::string* error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return fail(error, path + ": cannot open content package");
    auto size = input.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(content_bundle_max_bytes)) return fail(error, path + ": invalid content file size");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return fail(error, path + ": cannot read content package");
    // Catch concurrent growth too; publishers should install immutable files atomically.
    if (input.peek() != std::char_traits<char>::eof()) return fail(error, path + ": content file grew during read");
    if (input.bad()) return fail(error, path + ": cannot complete content file read");
    std::string diagnostic;
    if (!publish(bytes, expected_generation, &diagnostic)) return fail(error, path + ": " + diagnostic);
    if (error) error->clear();
    return true;
}
bool ContentStore::rollback(std::uint64_t expected_generation, std::string* error) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!current_ || current_->generation() != expected_generation) return fail(error, "Content generation conflict");
    if (!previous_) return fail(error, "No previous content snapshot");
    if (expected_generation == std::numeric_limits<std::uint64_t>::max()) return fail(error, "Content generation exhausted");
    auto restored = std::make_shared<ContentSnapshot>(*previous_);
    restored->generation_ = expected_generation + 1;
    current_ = std::move(restored);
    previous_.reset();
    if (error) error->clear();
    return true;
}

} } // namespace chwell::schema
