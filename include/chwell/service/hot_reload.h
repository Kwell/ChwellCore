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
// Legacy C++ component ABI: load before Service::Init/start, unload after stop.
// Service-bound reload/check_updates are rejected; use RuntimePluginStore for
// live byte request/response logic replacement. Matching C++ toolchain required.
// All access is on the Service owner thread; hooks must not reenter this manager.
// Service pins installed images through callback/component cleanup even when
// this manager is destroyed first. Failed Uninstall pins the image for the process.

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

    // Headless validation only. Bound Service plugins cannot be reloaded.
    bool reload(const std::string& path);

    // 扫描已加载插件的 mtime，变更则自动 reload
    // 返回成功重载的数量
    int check_updates();

    // 已加载插件路径
    std::vector<std::string> loaded_paths() const;

    bool is_loaded(const std::string& path) const;

    // Canonical directory containment (empty = unrestricted legacy behavior).
    void set_allowed_prefix(std::string prefix);

private:
    struct Loaded {
        std::string path;
        std::string name;
        int version = 0;
        std::shared_ptr<IPlugin> plugin;
        std::int64_t mtime = 0;
        bool installed = false;
    };

    bool load_internal(const std::string& path, Loaded& out);
    bool unload_internal(Loaded& item);
    static std::int64_t file_mtime(const std::string& path);
    bool path_allowed(const std::string& path) const;

    Service* service_;
    std::weak_ptr<int> service_lifetime_;
    std::string allowed_prefix_;
    mutable std::mutex mutex_;
    std::vector<Loaded> items_;
};

} // namespace service
} // namespace chwell
