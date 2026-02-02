#pragma once

#include "chwell/storage/storage_interface.h"
#include "chwell/storage/storage_types.h"

namespace chwell {
namespace storage {

// MySQL 存储实现：占位接口，需链接 MySQL 客户端库后实现
// 上层逻辑通过 StorageInterface 使用，不关心底层是 MySQL
class MysqlStorage : public StorageInterface {
public:
    explicit MysqlStorage(const StorageConfig& config);
    virtual ~MysqlStorage() override;

    virtual bool connect() override;
    virtual void disconnect() override;

    virtual StorageResult get(const std::string& key) override;
    virtual StorageResult put(const std::string& key, const std::string& value,
                              std::int64_t expire_at = 0) override;
    virtual StorageResult remove(const std::string& key) override;
    virtual bool exists(const std::string& key) override;

private:
    StorageConfig config_;
    void* conn_{nullptr};  // MYSQL* 占位，避免头文件依赖
};

}  // namespace storage
}  // namespace chwell
