#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace chwell {
namespace storage {

// 存储文档：上层逻辑使用统一结构，不关心底层介质
struct StorageDocument {
    std::string key;           // 主键，如 "player:123"、"room:456"
    std::string value;         // 值，通常为 JSON 或序列化数据
    std::int64_t expire_at{0}; // 过期时间戳（秒），0 表示永不过期

    StorageDocument() = default;
    StorageDocument(const std::string& k, const std::string& v,
                    std::int64_t exp = 0)
        : key(k), value(v), expire_at(exp) {}
};

// 存储操作结果：统一返回格式
struct StorageResult {
    bool ok{false};
    std::string error_msg;  // 失败时的错误信息
    std::string value;      // get 成功时的返回值

    static StorageResult success(const std::string& v = "") {
        StorageResult r;
        r.ok = true;
        r.value = v;
        return r;
    }

    static StorageResult failure(const std::string& msg) {
        StorageResult r;
        r.ok = false;
        r.error_msg = msg;
        return r;
    }
};

// 存储配置：不同介质使用不同参数，上层通过类型选择
struct StorageConfig {
    std::string type{"memory"};  // memory | mysql | mongodb
    std::string host{"127.0.0.1"};
    int port{0};
    std::string database;
    std::string user;
    std::string password;
    std::map<std::string, std::string> extra;  // 扩展参数
};

}  // namespace storage
}  // namespace chwell
