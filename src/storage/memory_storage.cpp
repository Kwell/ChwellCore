#include "chwell/storage/memory_storage.h"

namespace chwell {
namespace storage {

void MemoryStorage::prune_expired() {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    for (auto it = data_.begin(); it != data_.end();) {
        if (it->second.expire_at > 0 && it->second.expire_at < now) {
            it = data_.erase(it);
        } else {
            ++it;
        }
    }
}

StorageResult MemoryStorage::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired();
    auto it = data_.find(key);
    if (it == data_.end()) {
        return StorageResult::failure("key not found");
    }
    return StorageResult::success(it->second.value);
}

StorageResult MemoryStorage::put(const std::string& key, const std::string& value,
                                 std::int64_t expire_at) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_[key] = Entry{value, expire_at};
    return StorageResult::success();
}

StorageResult MemoryStorage::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) {
        return StorageResult::failure("key not found");
    }
    data_.erase(it);
    return StorageResult::success();
}

bool MemoryStorage::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired();
    return data_.find(key) != data_.end();
}

std::vector<std::string> MemoryStorage::keys(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_expired();
    std::vector<std::string> result;
    for (const auto& pair : data_) {
        if (prefix.empty() || pair.first.compare(0, prefix.size(), prefix) == 0) {
            result.push_back(pair.first);
        }
    }
    return result;
}

}  // namespace storage
}  // namespace chwell
