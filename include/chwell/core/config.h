#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <shared_mutex>
#include <functional>
#include <cstdint>

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
 * - 基础 key=value 格式 + 扁平 JSON（嵌套点号展开 a.b.c）
 * - 环境 profile：default + {env} 两层叠加
 * - 热加载：reload / check_reload（mtime）+ 变更回调
 * - 版本回滚：snapshot / rollback
 * - 键校验：require_keys
 * - 服务配置（server_name, bus_id）与组件配置列表
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

    // ========== JSON 配置 ==========
    // 扁平 JSON：{"a": {"b": 1}} 展开为 a.b=1；string/number/bool 均转字符串
    bool load_json_from_file(const std::string& path);
    // reset=true 时先清空已有 KV（整表替换）；false 时叠加覆盖（与 conf 混载）
    bool load_json_from_files(const std::vector<std::string>& paths, bool reset = true);

    // ========== 环境 profile ==========
    // 依次加载 default.{ext} 与 {env}.{ext}（ext 为 conf/json 自动探测）
    // 典型：load_for_env("conf", "prod") → conf/default.conf + conf/prod.conf
    bool load_for_env(const std::string& dir, const std::string& env);

    // ========== 热加载 ==========
    // 重新加载最后一次 load 的文件列表
    bool reload();
    // 检查文件 mtime，变化则自动 reload；返回是否发生重载
    bool check_reload();
    // 变更回调（reload 成功后触发，带本次生效的 key 列表为空表示整体刷新）
    void add_change_listener(std::function<void()> cb);

    // ========== 版本回滚 ==========
    // 将当前 kv_ 压入历史栈
    void snapshot();
    // 弹出最近快照并恢复；无快照返回 false
    bool rollback();

    // ========== 校验 ==========
    // 缺少任一 key 时返回 false 并写日志
    bool require_keys(const std::vector<std::string>& keys) const;

    // 最近一次加载的文件路径
    std::vector<std::string> loaded_files() const;

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
    
    // 不加锁版本，仅供已持有 mutex_ 的内部方法调用（避免重入死锁）
    std::string server_name_unlocked() const { return get_string_unlocked("server_name", "chwell_server"); }
    std::string bus_id_unlocked() const { return get_string_unlocked("bus_id", "8.8.8.1"); }
    
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
    // 返回副本：引用在解锁后失效，存在数据竞争/悬垂风险
    std::vector<ComponentConfig> components() const {
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
    bool parse_json_text(const std::string& text);
    static std::string detect_format(const std::string& path);  // "json" | "conf"
    void notify_change();
    void record_mtimes(const std::vector<std::string>& paths);
    static std::int64_t file_mtime(const std::string& path);  // 0 表示 stat 失败

    // 基础字段
    int listen_port_;
    int worker_threads_;

    // 通用配置键值表
    std::unordered_map<std::string, std::string> kv_;
    
    // 组件配置列表
    std::vector<ComponentConfig> components_;

    // 热加载 / 回滚
    std::vector<std::string> loaded_files_;
    std::unordered_map<std::string, std::int64_t> file_mtimes_;  // 每实例基线，避免跨对象串扰
    std::vector<std::unordered_map<std::string, std::string>> snapshots_;
    std::vector<std::function<void()>> change_listeners_;

    // 线程安全
    mutable std::shared_mutex mutex_;
};

} // namespace core
} // namespace chwell
