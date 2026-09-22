#pragma once

#include "chwell/storage/storage_interface.h"
#include "chwell/storage/storage_types.h"
#include <shared_mutex>

namespace chwell {
namespace storage {

// MongoDB 存储实现：条件编译，CHWELL_USE_MONGODB=ON 时链接 libmongoc
// 未开启时所有操作返回 failure("not built with MongoDB support")
class MongodbStorage : public StorageInterface {
public:
    explicit MongodbStorage(const StorageConfig& config);
    virtual ~MongodbStorage() override;

    virtual bool connect() override;
    virtual void disconnect() override;

    virtual StorageResult get(const std::string& key) override;
    virtual StorageResult put(const std::string& key, const std::string& value,
                              std::int64_t expire_at = 0) override;
    virtual StorageResult remove(const std::string& key) override;
    virtual bool exists(const std::string& key) override;
    virtual std::vector<std::string> keys(const std::string& prefix = "") override;

private:
    StorageConfig config_;
    void* client_{nullptr};       // mongoc_client_t*
    void* collection_{nullptr};   // mongoc_collection_t*
    mutable std::shared_mutex conn_mutex_;  // 保护并发访问
};

}  // namespace storage
}  // namespace chwell
