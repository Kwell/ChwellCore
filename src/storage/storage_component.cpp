#include "chwell/storage/storage_component.h"
#include "chwell/storage/storage_factory.h"

namespace chwell {
namespace storage {

StorageComponent::StorageComponent(std::unique_ptr<StorageInterface> storage)
    : storage_(std::move(storage)) {
    if (!storage_) {
        throw std::runtime_error("StorageComponent: storage is nullptr");
    }
}

StorageComponent::StorageComponent(const StorageConfig& config)
    : StorageComponent(StorageFactory::create(config)) {}

StorageComponent::StorageComponent(const std::string& yaml_path)
    : StorageComponent(StorageFactory::create_from_yaml(yaml_path)) {}

bool StorageComponent::Shut() {
    if (storage_) {
        storage_->disconnect();
    }
    return true;
}

StorageComponent::~StorageComponent() {
    if (storage_) {
        storage_->disconnect();
    }
}

}  // namespace storage
}  // namespace chwell
