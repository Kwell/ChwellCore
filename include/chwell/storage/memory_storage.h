#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>

#include "chwell/storage/storage_interface.h"
#include "chwell/storage/storage_types.h"

namespace chwell {
namespace storage {

// 内存存储实现：无外部依赖，适用于开发/测试
// 上层逻辑与使用 MySQL/MongoDB 时完全一致
class MemoryStorage : public StorageInterface {
public:
    MemoryStorage() = default;
    virtual ~MemoryStorage() override = default;

    virtual StorageResult get(const std::string& key) override;
    virtual StorageResult put(const std::string& key, const std::string& value,
                              std::int64_t expire_at = 0) override;
    virtual StorageResult remove(const std::string& key) override;
    virtual bool exists(const std::string& key) override;
    virtual std::vector<std::string> keys(const std::string& prefix = "") override;

private:
    void prune_expired();

    struct Entry {
        std::string value;
        std::int64_t expire_at{0};
        Entry() = default;
        Entry(const std::string& v, std::int64_t exp) : value(v), expire_at(exp) {}
    };
    std::unordered_map<std::string, Entry> data_;
    mutable std::mutex mutex_;
};

}  // namespace storage
}  // namespace chwell
