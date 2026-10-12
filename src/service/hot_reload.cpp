#include "chwell/service/hot_reload.h"
#include "chwell/service/plugin.h"
#include "chwell/service/service.h"
#include "chwell/core/logger.h"

#include <sys/stat.h>
#include <algorithm>
#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace chwell {
namespace service {

namespace {

void* dl_open(const char* path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::LoadLibraryA(path));
#else
    return ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* dl_sym(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return ::dlsym(handle, name);
#endif
}

void dl_close(void* handle) {
    if (!handle) return;
#if defined(_WIN32)
    ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

const char* dl_error() {
#if defined(_WIN32)
    return "LoadLibrary/GetProcAddress failed";
#else
    const char* e = ::dlerror();
    return e ? e : "unknown dlerror";
#endif
}

using CreateFn = IPlugin* (*)();
using DestroyFn = void (*)(IPlugin*);

} // namespace

HotReloadManager::HotReloadManager(PluginManager* plugins, Service* service)
    : service_(service), service_lifetime_(service ? service->dynamic_plugin_lifetime_ : std::weak_ptr<int>{}) { (void)plugins; }

HotReloadManager::~HotReloadManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : items_) {
        unload_internal(item);
    }
    items_.clear();
}

std::int64_t HotReloadManager::file_mtime(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<std::int64_t>(st.st_mtime);
}

bool HotReloadManager::path_allowed(const std::string& path) const {
    if (allowed_prefix_.empty()) return true;
    try {
        const auto root = std::filesystem::canonical(std::filesystem::u8path(allowed_prefix_));
        const auto candidate = std::filesystem::canonical(std::filesystem::u8path(path));
        auto part = candidate.begin();
        for (const auto& component : root) {
            if (part == candidate.end() || *part++ != component) return false;
        }
        return part != candidate.end();
    } catch (...) { return false; }
}

void HotReloadManager::set_allowed_prefix(std::string prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    allowed_prefix_ = std::move(prefix);
}

bool HotReloadManager::load_internal(const std::string& path, Loaded& out) {
    if (!path_allowed(path)) {
        CHWELL_LOG_ERROR("HotReload: path not allowed: " << path);
        return false;
    }

    void* handle = dl_open(path.c_str());
    if (!handle) {
        CHWELL_LOG_ERROR("HotReload: dlopen failed for " << path << ": " << dl_error());
        return false;
    }

    auto create = reinterpret_cast<CreateFn>(dl_sym(handle, "chwell_create_plugin"));
    auto destroy = reinterpret_cast<DestroyFn>(dl_sym(handle, "chwell_destroy_plugin"));
    if (!create || !destroy) {
        CHWELL_LOG_ERROR("HotReload: missing chwell_create_plugin/chwell_destroy_plugin in " << path);
        dl_close(handle);
        return false;
    }

    IPlugin* plugin = nullptr;
    try { plugin = create(); } catch (...) { dl_close(handle); return false; }
    if (!plugin) {
        CHWELL_LOG_ERROR("HotReload: create_plugin returned null for " << path);
        dl_close(handle);
        return false;
    }

    out.path = path;
    out.plugin = std::shared_ptr<IPlugin>(plugin, [destroy, handle](IPlugin* value) {
        try { destroy(value); }
        catch (...) { CHWELL_LOG_ERROR("HotReload: destroy threw; image stays resident"); return; }
        dl_close(handle);
    });
    try {
        out.name = plugin->GetName();
        out.version = plugin->GetVersion();
        if (out.name.empty() || out.version <= 0) return false;
    } catch (...) { return false; }
    out.mtime = file_mtime(path);
    out.installed = false;
    return true;
}

bool HotReloadManager::unload_internal(Loaded& item) {
    if (item.plugin && item.installed && service_ && !service_lifetime_.expired()) {
        if (!service_->uninstall_dynamic_plugin(item.plugin.get())) return false;
    }
    item.installed = false;
    item.plugin.reset();
    return true;
}

bool HotReloadManager::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (service_ && (service_lifetime_.expired() || !service_->dynamic_plugin_mutation_allowed())) return false;
    for (auto& item : items_) {
        if (item.path == path) {
            CHWELL_LOG_WARN("HotReload: already loaded " << path);
            return false;
        }
    }

    Loaded item;
    if (!load_internal(path, item)) return false;

    if (service_ && !service_->install_dynamic_plugin(item.plugin)) {
        CHWELL_LOG_ERROR("HotReload: Install failed for " << path);
        unload_internal(item);
        return false;
    }
    item.installed = service_ != nullptr;

    CHWELL_LOG_INFO("HotReload: loaded " << path << " (" << item.name
                    << " v" << item.version << ")");
    items_.push_back(std::move(item));
    return true;
}

bool HotReloadManager::unload(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(items_.begin(), items_.end(),
        [&](const Loaded& x) { return x.path == path; });
    if (it == items_.end()) {
        CHWELL_LOG_WARN("HotReload: not loaded " << path);
        return false;
    }
    if (!unload_internal(*it)) return false;
    items_.erase(it);
    CHWELL_LOG_INFO("HotReload: unloaded " << path);
    return true;
}

bool HotReloadManager::reload(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The Service component graph has no staging/commit or retained call handles.
    // Do not overlap two generations of C++ components or promise atomic rollback.
    if (service_) return false;

    auto it = std::find_if(items_.begin(), items_.end(),
        [&](const Loaded& x) { return x.path == path; });
    if (it == items_.end()) {
        CHWELL_LOG_WARN("HotReload: reload on non-loaded path " << path);
        return false;
    }

    // Headless legacy object recreation only. The OS may reuse this library;
    // live release switching belongs to RuntimePluginStore's isolated images.
    Loaded fresh;
    if (!load_internal(path, fresh)) {
        CHWELL_LOG_ERROR("HotReload: reload failed to open new image of " << path << ", keeping old");
        return false;
    }

    unload_internal(*it);

    *it = std::move(fresh);
    CHWELL_LOG_INFO("HotReload: reloaded " << path);
    return true;
}

int HotReloadManager::check_updates() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (service_) return 0;
    int reloaded = 0;
    for (auto& item : items_) {
        std::int64_t now = file_mtime(item.path);
        if (now != 0 && item.mtime != 0 && now != item.mtime) {
            CHWELL_LOG_INFO("HotReload: change detected for " << item.path
                            << " (mtime " << item.mtime << " -> " << now << ")");
            // unlock-safe: reload takes the same lock, so inline the logic
            Loaded fresh;
            if (!load_internal(item.path, fresh)) continue;
            unload_internal(item);
            item = std::move(fresh);
            ++reloaded;
        }
    }
    return reloaded;
}

std::vector<std::string> HotReloadManager::loaded_paths() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(items_.size());
    for (auto& item : items_) out.push_back(item.path);
    return out;
}

bool HotReloadManager::is_loaded(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : items_) {
        if (item.path == path) return true;
    }
    return false;
}

} // namespace service
} // namespace chwell
