#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace chwell {
namespace core {

class Config {
public:
    Config() : listen_port_(9000), worker_threads_(4) {}

    // 从单个配置文件加载（向后兼容原有接口）
    bool load_from_file(const std::string& path);

    // 从多个配置文件按顺序加载，后面的文件覆盖前面的同名键
    // 典型用法：["conf/default.conf", "conf/app.conf", "conf/app.local.conf"]
    bool load_from_files(const std::vector<std::string>& paths);

    // 基础字段（兼容现有示例）
    int listen_port() const { return listen_port_; }
    int worker_threads() const { return worker_threads_; }

    // 通用 KV 访问接口：支持通过 key 获取字符串或整数，含默认值
    std::string get_string(const std::string& key,
                           const std::string& default_value = std::string()) const;

    int get_int(const std::string& key, int default_value) const;

    // 直接设置键值（例如运行时注入）
    void set(const std::string& key, const std::string& value);

private:
    void apply_kv_to_fields();
    void apply_env_overrides();

    // 基础字段（仍然保留方便直接访问）
    int listen_port_;
    int worker_threads_;

    // 通用配置键值表（支持多文件覆盖 + 环境变量）
    std::unordered_map<std::string, std::string> kv_;
};

} // namespace core
} // namespace chwell

