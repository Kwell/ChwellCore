#pragma once

#include "chwell/schema/entity_schema.h"
#include <mutex>
#include <string_view>

namespace chwell { namespace schema {

// Server-side content only. The binary package includes private fields.
constexpr std::size_t content_bundle_max_bytes = 16 * 1024 * 1024;
constexpr std::size_t content_bundle_max_string = 1024 * 1024;
constexpr std::size_t content_bundle_max_tables = 64;
constexpr std::size_t content_bundle_max_rows = 100000; // across the whole package
constexpr std::size_t content_bundle_max_fields = 1024;

struct ContentReference {
    FieldId field;
    std::string table;
    bool allow_empty = false;
};
struct ContentTableDefinition {
    std::shared_ptr<const EntitySchema> schema;
    // Canonical normalized schema from the generator, compared byte-for-byte.
    std::string contract;
    std::vector<ContentReference> references;
};

class ContentRow {
public:
    const Value& value(FieldId id) const { return values_.at(id); }
    const EntitySchema& definition() const { return *schema_; }
private:
    friend class ContentStore;
    ContentRow() = default;
    std::shared_ptr<const EntitySchema> schema_;
    std::map<FieldId, Value> values_;
};

// Retain this handle for an entire logical operation/tick to keep all tables
// on one version. Rows/references remain valid as long as the handle is held.
class ContentSnapshot {
public:
    std::uint64_t version() const { return version_; }
    std::uint64_t generation() const { return generation_; }
    const ContentRow* find(const std::string& table, const std::string& id) const;
    const std::map<std::string, ContentRow>& rows(const std::string& table) const;
private:
    friend class ContentStore;
    ContentSnapshot() = default;
    using Tables = std::map<std::string, std::map<std::string, ContentRow>>;
    std::uint64_t version_ = 0, generation_ = 0;
    std::shared_ptr<const Tables> tables_;
};

class ContentStore {
public:
    explicit ContentStore(std::vector<ContentTableDefinition> definitions);
    std::shared_ptr<const ContentSnapshot> snapshot() const;
    // Entire package is decoded and validated before publication. Schema must
    // match the compiled contract; all expected tables/fields must be present.
    // expected_generation is 0 before the initial publication. Versions must
    // exceed every previously published version, even after rollback.
    bool publish(std::string_view bytes, std::uint64_t expected_generation,
                 std::string* error = nullptr);
    bool publish_file(const std::string& path, std::uint64_t expected_generation,
                      std::string* error = nullptr);
    // One retained predecessor; successful rollback consumes that predecessor
    // and increments generation while restoring its original content version.
    bool rollback(std::uint64_t expected_generation, std::string* error = nullptr);
private:
    const std::map<std::string, ContentTableDefinition> definitions_;
    mutable std::mutex mutex_;
    std::shared_ptr<const ContentSnapshot> current_, previous_;
    std::uint64_t highest_version_ = 0;
};

} } // namespace chwell::schema
