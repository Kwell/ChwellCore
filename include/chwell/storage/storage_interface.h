#pragma once

#include <string>
#include <vector>
#include <memory>

#include "chwell/storage/storage_types.h"

namespace chwell {
namespace storage {

// 存储接口：上层逻辑只依赖此接口，不关心底层是 MySQL、MongoDB 或其他介质
class StorageInterface {
public:
    virtual ~StorageInterface() {}

    // 获取单条数据
    virtual StorageResult get(const std::string& key) = 0;

    // 写入单条数据
    virtual StorageResult put(const std::string& key, const std::string& value,
                              std::int64_t expire_at = 0) = 0;

    // 写入文档（含过期时间）
    virtual StorageResult put(const StorageDocument& doc) {
        return put(doc.key, doc.value, doc.expire_at);
    }

    // 删除单条数据
    virtual StorageResult remove(const std::string& key) = 0;

    // 检查 key 是否存在
    virtual bool exists(const std::string& key) = 0;

    // 按前缀列出 key（可选，部分实现可能不支持）
    virtual std::vector<std::string> keys(const std::string& prefix = "") {
        (void)prefix;
        return {};
    }

    // 批量获取（可选，默认逐条调用 get）
    virtual std::vector<StorageResult> mget(const std::vector<std::string>& keys) {
        std::vector<StorageResult> results;
        for (const auto& k : keys) {
            results.push_back(get(k));
        }
        return results;
    }

    // 批量写入（可选，默认逐条调用 put）
    virtual StorageResult mput(const std::vector<StorageDocument>& docs) {
        for (const auto& doc : docs) {
            auto r = put(doc);
            if (!r.ok) return r;
        }
        return StorageResult::success();
    }

    // 初始化/连接（部分实现需要）
    virtual bool connect() { return true; }

    // 断开连接
    virtual void disconnect() {}
};

}  // namespace storage
}  // namespace chwell
