#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <shared_mutex>

namespace chwell {
namespace core {

/**
 * @brief 组件配置
 */
struct ComponentConfig {
    std::string name;       // 组件名称
    bool enabled = true;    // 是否启用
    int priority = 100;     // 优先级（数字越小越先初始化）
    std::unordered_map<std::string, std::string> params; // 组件参数
};

/**
 * @brief 配置管理类
 * 
 * 支持：
 * - 基础 key=value 格式
 * - 服务配置（server_name, bus_id）
 * - 组件配置列表
 * - 环境变量覆盖
 */
class Config {
public:
    Config() : listen_port_(9000), worker_threads_(4) {}

    // 从单个配置文件加载（向后兼容原有接口）
    bool load_from_file(const std::string& path);

    // 从多个配置文件按顺序加载，后面的文件覆盖前面的同名键
    // 典型用法：["conf/default.conf", "conf/app.conf", "conf/app.local.conf"]
    bool load_from_files(const std::vector<std::string>& paths);

    // ========== 基础字段 ==========
    
    int listen_port() const {
        std::shared_lock lock(mutex_);
        return listen_port_;
    }
    int worker_threads() const {
        std::shared_lock lock(mutex_);
        return worker_threads_;
    }
    
    // ========== 服务配置 ==========
    
    std::string server_name() const { return get_string("server_name", "chwell_server"); }
    std::string bus_id() const { return get_string("bus_id", "8.8.8.1"); }
    
    // ========== 通用 KV 访问 ==========
    
    std::string get_string(const std::string& key,
                           const std::string& default_value = std::string()) const;

    int get_int(const std::string& key, int default_value) const;
    
    bool get_bool(const std::string& key, bool default_value) const;

    std::string get_string_unlocked(const std::string& key,
                           const std::string& default_value = std::string()) const;

    int get_int_unlocked(const std::string& key, int default_value) const;
    
    bool get_bool_unlocked(const std::string& key, bool default_value) const;

    void set(const std::string& key, const std::string& value);

    // ========== 组件配置 ==========
    
    // 获取组件配置列表
    const std::vector<ComponentConfig>& components() const {
        std::shared_lock lock(mutex_);
        return components_;
    }
    
    // 检查组件是否启用
    bool is_component_enabled(const std::string& name) const;
    
    // 获取组件优先级
    int get_component_priority(const std::string& name) const;

private:
    void apply_kv_to_fields();
    void apply_env_overrides();
    void parse_components();

    // 基础字段
    int listen_port_;
    int worker_threads_;

    // 通用配置键值表
    std::unordered_map<std::string, std::string> kv_;
    
    // 组件配置列表
    std::vector<ComponentConfig> components_;

    // 线程安全
    mutable std::shared_mutex mutex_;
};

} // namespace core
} // namespace chwell
