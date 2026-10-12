#pragma once

#include "chwell/schema/entity_schema.h"
#include "chwell/storage/storage_types.h"
#include <functional>

namespace chwell { namespace schema {

// Keep this receipt outside the database until execution is confirmed. Both
// directions use MysqlSessionStore::compare_exchange, never unconditional put.
struct MigrationChange {
    storage::StorageDocument before, after;
};

class StorageMigration {
public:
    using Transform = std::function<bool(const storage::orm::Document& source,
        storage::orm::Document& destination, std::string* error)>;
    // Explicitly registered, increasing version step; no automatic field casts.
    StorageMigration(std::shared_ptr<const EntitySchema> source,
                     std::shared_ptr<const EntitySchema> destination, Transform transform);
    // Pure dry-run. Unknown fields, malformed/noncanonical serialized documents,
    // bad versions, invalid values and identity changes fail without replacing out.
    // Exceptions from user transforms propagate; no storage is touched.
    bool prepare(const storage::StorageDocument& input, MigrationChange& out,
                 std::string* error = nullptr) const;
private:
    std::shared_ptr<const EntitySchema> source_, destination_;
    Transform transform_;
};

} }
