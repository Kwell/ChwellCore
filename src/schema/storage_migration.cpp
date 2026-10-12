#include "chwell/schema/storage_migration.h"
#include <stdexcept>

namespace chwell { namespace schema {
namespace {
bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}
bool known_fields(const EntitySchema& schema, const storage::orm::Document& doc,
                  std::string* error) {
    for (const auto& entry : doc.data()) {
        if (entry.first == storage_version_field) continue;
        bool known = false;
        for (const auto& field : schema.fields()) {
            if (field.stored && field.name == entry.first) { known = true; break; }
        }
        if (!known) return fail(error, "Unknown stored field: " + entry.first);
    }
    return true;
}
}

StorageMigration::StorageMigration(std::shared_ptr<const EntitySchema> source,
    std::shared_ptr<const EntitySchema> destination, Transform transform)
    : source_(std::move(source)), destination_(std::move(destination)), transform_(std::move(transform)) {
    if (!source_ || !destination_ || !transform_ || !source_->version() ||
        destination_->version() <= source_->version() || source_->name() != destination_->name() ||
        source_->table() != destination_->table() || source_->key() != destination_->key())
        throw std::invalid_argument("Migration requires matching entity identity and increasing nonzero versions");
}

bool StorageMigration::prepare(const storage::StorageDocument& input, MigrationChange& out,
    std::string* error) const {
    storage::orm::Document source;
    // The legacy parser accepts some lossy input. A canonical round trip proves
    // that no duplicate key, junk line or ambiguous escape was silently dropped.
    if (!source.from_string(input.value) || source.to_string() != input.value)
        return fail(error, "Migration requires canonical Document serialization");
    if (!known_fields(*source_, source, error)) return false;
    SchemaEntity old(source_);
    if (!old.load_document(source, error)) return false;
    if (input.key != source_->table() + ":" + old.id())
        return fail(error, "Storage key and entity identity differ");
    auto destination = old.to_document(); // Materialize old defaults before transformation.
    destination.erase(storage_version_field);
    if (!transform_(source, destination, error)) return false;
    if (destination.has(storage_version_field)) return fail(error, "Transform must not set version metadata");
    if (!known_fields(*destination_, destination, error)) return false;
    destination.set_string(storage_version_field, std::to_string(destination_->version()));
    SchemaEntity next(destination_);
    if (!next.load_document(destination, error)) return false;
    if (next.id() != old.id()) return fail(error, "Migration cannot change entity identity");
    MigrationChange pending{input, {input.key, next.to_document().to_string(), input.expire_at}};
    out = std::move(pending);
    if (error) error->clear();
    return true;
}

} }
