#pragma once

#include "chwell/storage/orm/writeback_cache.h"
#include <variant>
#include <optional>
#include <unordered_set>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace chwell { namespace schema {

using FieldId = std::uint32_t;
using Value = std::variant<std::int64_t, double, bool, std::string>;
enum class FieldType { Int64, Double, Bool, String };
enum class Visibility { Server, Owner, Public };

struct FieldDefinition {
    FieldId id = 0;
    std::string name;
    FieldType type = FieldType::String;
    Value default_value = std::string();
    bool stored = false;
    Visibility visibility = Visibility::Server;
    std::optional<std::int64_t> int_min, int_max;
    std::optional<double> double_min, double_max;
    std::optional<std::size_t> max_bytes;
};

struct FieldValue {
    FieldId id;
    Value value;
    bool operator==(const FieldValue& other) const { return id == other.id && value == other.value; }
};

// Immutable metadata shared by generated typed entities, storage and sync.
// Deleted IDs remain reserved. The key is a public stored string field named "id" to
// match the existing Repository convention. No runtime reflection dependency.
class EntitySchema {
public:
    EntitySchema(std::string name, std::string table, FieldId key,
                 std::vector<FieldDefinition> fields, std::vector<FieldId> reserved = {});
    const std::string& name() const { return name_; }
    const std::string& table() const { return table_; }
    FieldId key() const { return key_; }
    const std::vector<FieldDefinition>& fields() const { return fields_; }
    const FieldDefinition* find(FieldId id) const;
    bool validate(FieldId id, const Value& value, std::string* error = nullptr) const;
    static bool visible(const FieldDefinition& field, bool is_owner);
private:
    std::string name_, table_;
    FieldId key_;
    std::vector<FieldDefinition> fields_;
};

// Optional base for generated entities. Existing Entity/PersistableEntity code
// remains usable. All mutation, load and sync must run on the owning logic thread.
class SchemaEntity : public storage::orm::PersistableEntity {
public:
    explicit SchemaEntity(std::shared_ptr<const EntitySchema> schema);
    const EntitySchema& definition() const { return *schema_; }
    const Value& value(FieldId id) const;
    bool set_value(FieldId id, Value value, std::string* error = nullptr);
    std::string table_name() const override { return schema_->table(); }
    std::string id() const override;
    storage::orm::Document to_document() const override;
    void from_document(const storage::orm::Document& doc) override;
    bool load_document(const storage::orm::Document& doc, std::string* error = nullptr);
    std::vector<FieldValue> snapshot(bool is_owner) const;
private:
    std::shared_ptr<const EntitySchema> schema_;
    std::map<FieldId, Value> values_;
};

} } // namespace chwell::schema
