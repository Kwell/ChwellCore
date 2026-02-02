#include "chwell/storage/storage_component.h"
#include "chwell/storage/storage_factory.h"

namespace chwell {
namespace storage {

StorageComponent::StorageComponent(std::unique_ptr<StorageInterface> storage)
    : storage_(std::move(storage)) {}

StorageComponent::StorageComponent(const StorageConfig& config)
    : storage_(StorageFactory::create(config)) {}

StorageComponent::StorageComponent(const std::string& yaml_path)
    : storage_(StorageFactory::create_from_yaml(yaml_path)) {}

StorageComponent::~StorageComponent() {
    if (storage_) {
        storage_->disconnect();
    }
}

}  // namespace storage
}  // namespace chwell
