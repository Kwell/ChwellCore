#include "chwell/service/plugin.h"
#include "chwell/service/service.h"
#include "chwell/core/logger.h"
#include <algorithm>

namespace chwell {
namespace service {

bool PluginManager::InstallAll(Service& service) {
    // 按优先级排序
    std::sort(plugins_.begin(), plugins_.end(),
        [](const std::unique_ptr<IPlugin>& a, const std::unique_ptr<IPlugin>& b) {
            return a->GetPriority() < b->GetPriority();
        });
    
    CHWELL_LOG_INFO("PluginManager: installing " << plugins_.size() << " plugins...");
    
    for (auto& plugin : plugins_) {
        if (!plugin) continue;
        
        CHWELL_LOG_INFO("PluginManager: installing " << plugin->GetName() 
            << " (v" << plugin->GetVersion() << ")...");
        
        if (!plugin->Install(service)) {
            CHWELL_LOG_ERROR("PluginManager: " << plugin->GetName() << " Install failed!");
            return false;
        }
        
        CHWELL_LOG_INFO("PluginManager: " << plugin->GetName() << " installed successfully");
    }
    
    CHWELL_LOG_INFO("PluginManager: all plugins installed");
    return true;
}

bool PluginManager::UninstallAll(Service& service) {
    CHWELL_LOG_INFO("PluginManager: uninstalling " << plugins_.size() << " plugins...");
    
    // 逆序卸载
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        auto& plugin = *it;
        if (!plugin) continue;
        
        CHWELL_LOG_INFO("PluginManager: uninstalling " << plugin->GetName() << "...");
        
        if (!plugin->Uninstall(service)) {
            CHWELL_LOG_ERROR("PluginManager: " << plugin->GetName() << " Uninstall failed!");
            // 继续尝试卸载其他插件
        }
    }
    
    plugins_.clear();
    CHWELL_LOG_INFO("PluginManager: all plugins uninstalled");
    return true;
}

} // namespace service
} // namespace chwell