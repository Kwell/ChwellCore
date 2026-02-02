#pragma once

#include "chwell/storage/storage_interface.h"
#include "chwell/storage/storage_types.h"

namespace chwell {
namespace storage {

// MongoDB 存储实现：占位接口，需链接 mongocxx 后实现
// 上层逻辑通过 StorageInterface 使用，不关心底层是 MongoDB
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
    void* client_{nullptr};   // mongocxx::client* 占位
    void* collection_{nullptr};  // mongocxx::collection* 占位
};

}  // namespace storage
}  // namespace chwell
