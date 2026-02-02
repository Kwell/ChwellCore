#pragma once

#include <memory>
#include <string>

#include "chwell/storage/storage_interface.h"
#include "chwell/storage/storage_types.h"
#include "chwell/storage/orm/repository.h"
#include "chwell/service/component.h"

namespace chwell {
namespace storage {

// StorageComponent：将存储挂载到 Service，供业务组件通过 get_component<StorageComponent>() 使用
// 上层逻辑只依赖 StorageInterface，不关心底层是 MySQL 还是 MongoDB
class StorageComponent : public service::Component {
public:
    // 使用已有存储实例
    explicit StorageComponent(std::unique_ptr<StorageInterface> storage);

    // 根据配置创建存储
    explicit StorageComponent(const StorageConfig& config);

    // 从 YAML 配置文件创建（推荐，介质和连接参数均在 YAML 中）
    explicit StorageComponent(const std::string& yaml_path);

    virtual ~StorageComponent() override;

    virtual std::string name() const override {
        return "StorageComponent";
    }

    // 获取存储接口，业务逻辑通过此接口操作
    StorageInterface* storage() { return storage_.get(); }
    const StorageInterface* storage() const { return storage_.get(); }

    // 便捷方法：直接委托给 storage_
    StorageResult get(const std::string& key) {
        return storage_ ? storage_->get(key) : StorageResult::failure("storage not initialized");
    }
    StorageResult put(const std::string& key, const std::string& value,
                      std::int64_t expire_at = 0) {
        return storage_ ? storage_->put(key, value, expire_at)
                       : StorageResult::failure("storage not initialized");
    }
    StorageResult remove(const std::string& key) {
        return storage_ ? storage_->remove(key)
                       : StorageResult::failure("storage not initialized");
    }
    bool exists(const std::string& key) {
        return storage_ ? storage_->exists(key) : false;
    }

    // ORM 仓储：类型安全的 CRUD，无需手写 key-value
    template <typename T>
    orm::Repository<T> repository(const std::string& table_name) {
        return orm::Repository<T>(storage_.get(), table_name);
    }

private:
    std::unique_ptr<StorageInterface> storage_;
};

}  // namespace storage
}  // namespace chwell
