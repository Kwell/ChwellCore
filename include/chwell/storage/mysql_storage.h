#pragma once

#include "chwell/storage/storage_interface.h"
#include "chwell/storage/storage_types.h"
#include <shared_mutex>
#include <string>
#include <cctype>

namespace chwell {
namespace storage {

// MySQL 存储实现：条件编译，CHWELL_USE_MYSQL=ON 时链接 libmysqlclient
// 未开启时所有操作返回 failure("not built with MySQL support")
// 上层逻辑通过 StorageInterface 使用，不关心底层介质
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
    // 按前缀列举 key，支持 Repository::find_all()
    virtual std::vector<std::string> keys(const std::string& prefix = "") override;

    // 校验表名只允许 [a-zA-Z0-9_]，不合法时回退到 "kv"
    static std::string sanitize_table_name(const std::string& name) {
        if (name.empty()) return "kv";
        for (char c : name) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return "kv";
        }
        return name;
    }

private:
    // 尝试重连，成功返回 true；失败返回 false
    bool ensure_connected();

    StorageConfig config_;
    void* conn_{nullptr};  // MYSQL* 不暴露到头文件，避免依赖 mysql.h
    mutable std::shared_mutex conn_mutex_;
};

}  // namespace storage
}  // namespace chwell
