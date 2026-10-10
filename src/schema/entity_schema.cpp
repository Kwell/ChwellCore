#include "chwell/schema/entity_schema.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <limits>
#include <stdexcept>
#include <sstream>

namespace chwell { namespace schema {
namespace {
bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}
bool fits(const FieldDefinition& field, const Value& value) {
    switch (field.type) {
    case FieldType::Int64:
        return std::holds_alternative<std::int64_t>(value) &&
            (!field.int_min || std::get<std::int64_t>(value) >= *field.int_min) &&
            (!field.int_max || std::get<std::int64_t>(value) <= *field.int_max);
    case FieldType::Double:
        return std::holds_alternative<double>(value) && std::isfinite(std::get<double>(value)) &&
            (!field.double_min || std::get<double>(value) >= *field.double_min) &&
            (!field.double_max || std::get<double>(value) <= *field.double_max);
    case FieldType::Bool: return std::holds_alternative<bool>(value);
    case FieldType::String:
        return std::holds_alternative<std::string>(value) &&
            (!field.max_bytes || std::get<std::string>(value).size() <= *field.max_bytes);
    }
    return false;
}
std::string encode(const Value& value) {
    if (const auto* v = std::get_if<std::string>(&value)) return *v;
    if (const auto* v = std::get_if<std::int64_t>(&value)) return std::to_string(*v);
    if (const auto* v = std::get_if<bool>(&value)) return *v ? "true" : "false";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << std::get<double>(value);
    return out.str();
}
bool decode(FieldType type, const std::string& text, Value& out) {
    switch (type) {
    case FieldType::String: out = text; return true;
    case FieldType::Bool:
        if (text == "true" || text == "1") { out = true; return true; }
        if (text == "false" || text == "0") { out = false; return true; }
        return false;
    case FieldType::Int64: {
        std::int64_t value = 0;
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) return false;
        out = value; return true;
    }
    case FieldType::Double: {
        std::istringstream input(text);
        input.imbue(std::locale::classic());
        double value = 0;
        input >> std::noskipws >> value;
        if (!input || !input.eof() || !std::isfinite(value)) return false;
        out = value; return true;
    }
    }
    return false;
}
} // namespace

EntitySchema::EntitySchema(std::string name, std::string table, FieldId key,
    std::vector<FieldDefinition> fields, std::vector<FieldId> reserved)
    : name_(std::move(name)), table_(std::move(table)), key_(key), fields_(std::move(fields)) {
    if (name_.empty() || table_.empty()) throw std::invalid_argument("Schema name/table is empty");
    std::unordered_set<FieldId> ids;
    std::unordered_set<std::string> names;
    for (auto id : reserved) {
        if (!id || !ids.insert(id).second) throw std::invalid_argument("Invalid reserved field ID");
    }
    for (const auto& field : fields_) {
        if (!field.id || field.name.empty() || !ids.insert(field.id).second || !names.insert(field.name).second) {
            throw std::invalid_argument("Duplicate, reserved or empty schema field");
        }
        if ((field.int_min || field.int_max) && field.type != FieldType::Int64) throw std::invalid_argument("Integer constraint type mismatch");
        if ((field.double_min || field.double_max) && field.type != FieldType::Double) throw std::invalid_argument("Double constraint type mismatch");
        if (field.max_bytes && field.type != FieldType::String) throw std::invalid_argument("String constraint type mismatch");
        if ((field.int_min && field.int_max && *field.int_min > *field.int_max) ||
            (field.double_min && !std::isfinite(*field.double_min)) ||
            (field.double_max && !std::isfinite(*field.double_max)) ||
            (field.double_min && field.double_max && *field.double_min > *field.double_max) || !fits(field, field.default_value)) {
            throw std::invalid_argument("Invalid schema default or constraints: " + field.name);
        }
        if (field.visibility != Visibility::Server && field.visibility != Visibility::Owner && field.visibility != Visibility::Public) {
            throw std::invalid_argument("Invalid schema visibility");
        }
    }
    std::sort(fields_.begin(), fields_.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    const auto* identity = find(key_);
    if (!identity || identity->name != "id" || identity->type != FieldType::String || !identity->stored ||
        !std::get<std::string>(identity->default_value).empty() || identity->visibility != Visibility::Public ||
        (identity->max_bytes && *identity->max_bytes == 0)) {
        throw std::invalid_argument("Schema key must be a public stored string field named id with empty default and room for a nonempty identity");
    }
}

const FieldDefinition* EntitySchema::find(FieldId id) const {
    auto it = std::lower_bound(fields_.begin(), fields_.end(), id, [](const auto& field, FieldId key) { return field.id < key; });
    return it != fields_.end() && it->id == id ? &*it : nullptr;
}
bool EntitySchema::validate(FieldId id, const Value& value, std::string* error) const {
    const auto* field = find(id);
    if (!field) return fail(error, "Unknown field ID " + std::to_string(id));
    if (!fits(*field, value)) return fail(error, "Field type or constraint violation: " + field->name);
    if (error) error->clear();
    return true;
}
bool EntitySchema::visible(const FieldDefinition& field, bool is_owner) {
    return field.visibility == Visibility::Public || (is_owner && field.visibility == Visibility::Owner);
}

SchemaEntity::SchemaEntity(std::shared_ptr<const EntitySchema> schema) : schema_(std::move(schema)) {
    if (!schema_) throw std::invalid_argument("SchemaEntity requires metadata");
    for (const auto& field : schema_->fields()) values_.emplace(field.id, field.default_value);
}
const Value& SchemaEntity::value(FieldId id) const { return values_.at(id); }
bool SchemaEntity::set_value(FieldId id, Value value, std::string* error) {
    if (!schema_->validate(id, value, error)) return false;
    auto& previous = values_.at(id);
    if (previous == value) return true;
    // A nonempty primary key cannot be changed by a setter. Loading an entity
    // is a separate operation and must precede registering it with a sync room.
    if (id == schema_->key() && !std::get<std::string>(previous).empty()) return fail(error, "Entity key is immutable");
    const auto* field = schema_->find(id);
    if (field->stored) mark_dirty(field->name);
    previous = std::move(value);
    return true;
}
std::string SchemaEntity::id() const { return std::get<std::string>(value(schema_->key())); }
storage::orm::Document SchemaEntity::to_document() const {
    storage::orm::Document document;
    for (const auto& field : schema_->fields()) {
        if (field.stored) document.set_string(field.name, encode(value(field.id)));
    }
    return document;
}
bool SchemaEntity::load_document(const storage::orm::Document& doc, std::string* error) {
    std::map<FieldId, Value> pending;
    for (const auto& field : schema_->fields()) {
        Value value = field.default_value;
        if (field.stored && doc.has(field.name) && !decode(field.type, doc.get_string(field.name), value)) {
            return fail(error, "Invalid stored value: " + field.name);
        }
        if (!schema_->validate(field.id, value, error)) return false;
        pending.emplace(field.id, std::move(value));
    }
    if (std::get<std::string>(pending.at(schema_->key())).empty()) return fail(error, "Stored entity ID is empty");
    values_.swap(pending);
    clear_dirty();
    if (error) error->clear();
    return true;
}
void SchemaEntity::from_document(const storage::orm::Document& doc) {
    std::string error;
    if (!load_document(doc, &error)) throw std::invalid_argument(error);
}
std::vector<FieldValue> SchemaEntity::snapshot(bool is_owner) const {
    std::vector<FieldValue> result;
    for (const auto& field : schema_->fields()) {
        if (EntitySchema::visible(field, is_owner)) result.push_back({field.id, value(field.id)});
    }
    return result;
}

} } // namespace chwell::schema
