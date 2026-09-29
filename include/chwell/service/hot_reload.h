#pragma once

// 动态库热加载：在不重启进程的情况下加载/卸载/重载插件
//
// 插件 ABI（必须导出两个 C 符号）：
//   extern "C" chwell::service::IPlugin* chwell_create_plugin();
//   extern "C" void chwell_destroy_plugin(chwell::service::IPlugin*);
//
// 用法：
//   service::HotReloadManager hot(&plugin_mgr, &svc);
//   hot.load("./plugins/game_logic.so");
//   // 运行时：文件 mtime 变化后
//   hot.check_updates();   // 自动重载变更的插件
//
// 安全换插件流程（reload）：
//   1. dlopen 新库（旧库仍驻留，避免符号被卸载）
//   2. 创建新插件实例并 Install
//   3. Uninstall 旧插件并 destroy
//   4. dlclose 旧库
// 任一步失败则回滚到旧插件。

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <cstdint>

namespace chwell {
namespace service {

class Service;
class IPlugin;
class PluginManager;

class HotReloadManager {
public:
    HotReloadManager(PluginManager* plugins, Service* service);
    ~HotReloadManager();

    HotReloadManager(const HotReloadManager&) = delete;
    HotReloadManager& operator=(const HotReloadManager&) = delete;

    // 加载插件库；同路径重复加载返回 false
    bool load(const std::string& path);

    // 卸载并释放
    bool unload(const std::string& path);

    // 强制重载（先装新再卸旧，失败回滚）
    bool reload(const std::string& path);

    // 扫描已加载插件的 mtime，变更则自动 reload
    // 返回成功重载的数量
    int check_updates();

    // 已加载插件路径
    std::vector<std::string> loaded_paths() const;

    bool is_loaded(const std::string& path) const;

    // 允许加载的目录前缀（空 = 不限制）。生产建议限定到 plugins/ 目录
    void set_allowed_prefix(std::string prefix) { allowed_prefix_ = std::move(prefix); }

private:
    struct Loaded {
        std::string path;
        void* handle = nullptr;
        IPlugin* plugin = nullptr;
        void (*destroy_fn)(IPlugin*) = nullptr;
        std::int64_t mtime = 0;
        bool installed = false;
    };

    bool load_internal(const std::string& path, Loaded& out);
    void unload_internal(Loaded& item);
    static std::int64_t file_mtime(const std::string& path);
    bool path_allowed(const std::string& path) const;

    PluginManager* plugins_;
    Service* service_;
    std::string allowed_prefix_;
    mutable std::mutex mutex_;
    std::vector<Loaded> items_;
};

} // namespace service
} // namespace chwell
