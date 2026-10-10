#include "chwell/service/plugin.h"
#include "chwell/service/service.h"
#include "chwell/core/logger.h"
#include <algorithm>

namespace chwell {
namespace service {

bool PluginManager::InstallAll(Service& service) {
    if (installed_) return installed_service_ == &service;
    if (installing_ || uninstalling_ || service.running_ || !service.initialized_components_.empty() ||
        service.registration_owner_) return false;
    std::stable_sort(plugins_.begin(), plugins_.end(),
        [](const std::unique_ptr<IPlugin>& a, const std::unique_ptr<IPlugin>& b) {
            return a->GetPriority() < b->GetPriority();
        });
    
    installed_plugins_.reserve(plugins_.size());
    installing_ = true;
    installed_service_ = &service;
    
    for (auto& plugin : plugins_) {
        service.registration_owner_ = plugin.get();
        service.registration_error_ = false;
        // Include a failed installation so its partial resources are unwound.
        installed_plugins_.push_back(plugin.get());
        bool ok = false;
        try {
            ok = plugin->Install(service) && !service.registration_error_;
        } catch (...) {
            CHWELL_LOG_ERROR("PluginManager: Install threw for " << plugin->GetName());
        }
        service.registration_owner_ = nullptr;
        if (!ok) {
            CHWELL_LOG_ERROR("PluginManager: Install failed for " << plugin->GetName());
            installing_ = false;
            UninstallAll(service);
            return false;
        }
    }
    
    installing_ = false;
    installed_ = true;
    return true;
}

bool PluginManager::UninstallAll(Service& service) {
    if (installing_ || uninstalling_ || service.running_ || !service.initialized_components_.empty()) return false;
    if (installed_service_ && installed_service_ != &service) return false;
    bool ok = true;
    uninstalling_ = true;
    const bool was_blocked = service.registration_blocked_;
    service.registration_blocked_ = true;
    for (auto it = installed_plugins_.rbegin(); it != installed_plugins_.rend(); ++it) {
        service.shutdown_owned_components(*it);
        try {
            if (!(*it)->Uninstall(service)) ok = false;
        } catch (...) {
            CHWELL_LOG_ERROR("PluginManager: Uninstall threw for " << (*it)->GetName());
            ok = false;
        }
        service.remove_owned_components(*it);
    }
    
    installed_plugins_.clear();
    installed_ = false;
    installed_service_ = nullptr;
    uninstalling_ = false;
    service.registration_blocked_ = was_blocked;
    return ok;
}

} // namespace service
} // namespace chwell
