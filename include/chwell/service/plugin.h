#pragma once

#include <string>
#include <vector>
#include <memory>

namespace chwell {
namespace service {

class Service; // 前向声明
class Component; // 前向声明

/**
 * @brief 插件接口：管理一组相关的组件
 * 
 * 设计理念：
 * - Plugin 是组件的容器，管理一组相关的组件
 * - Service 通过 Plugin 间接管理组件
 * - 支持模块化开发和部署
 * 
 * 使用流程：
 * 1. 定义 Plugin 类继承 IPlugin
 * 2. 在 Install() 中注册组件
 * 3. 在 Uninstall() 中清理组件
 * 
 * 示例：
 * class GamePlugin : public IPlugin {
 * public:
 *     const std::string& GetName() const override {
 *         static std::string name = "GamePlugin";
 *         return name;
 *     }
 *     
 *     bool Install(Service& service) override {
 *         service.add_component<PlayerManager>();
 *         service.add_component<BattleManager>();
 *         return true;
 *     }
 *     
 *     bool Uninstall(Service& service) override {
 *         // 清理工作
 *         return true;
 *     }
 * };
 */
class IPlugin {
public:
    virtual ~IPlugin() = default;
    
    /**
     * @brief 获取插件名称
     */
    virtual const std::string& GetName() const = 0;
    
    /**
     * @brief 获取插件版本
     */
    virtual int GetVersion() const { return 1; }
    
    /**
     * @brief 安装插件：注册组件到 Service
     * @param service 服务实例
     * @return 安装成功返回 true
     */
    virtual bool Install(Service& service) { return true; }
    
    /**
     * @brief 卸载插件：清理组件
     * @param service 服务实例
     * @return 卸载成功返回 true
     */
    virtual bool Uninstall(Service& service) { return true; }
    
    /**
     * @brief 获取插件优先级（数字越小越先加载）
     */
    virtual int GetPriority() const { return 100; }
};

/**
 * @brief 插件管理器：管理所有插件的生命周期
 * 
 * 支持：
 * - 动态注册插件
 * - 按优先级加载插件
 * - 统一的生命周期管理
 */
class PluginManager {
public:
    PluginManager() = default;
    ~PluginManager() = default;
    
    /**
     * @brief 注册插件
     * @param plugin 插件实例
     */
    void RegisterPlugin(std::unique_ptr<IPlugin> plugin) {
        if (!plugin) return;
        plugins_.push_back(std::move(plugin));
    }
    
    /**
     * @brief 模板方法：创建并注册插件
     */
    template<typename PluginType, typename... Args>
    void RegisterPlugin(Args&&... args) {
        static_assert(std::is_base_of<IPlugin, PluginType>::value,
                      "PluginType must derive from IPlugin");
        auto plugin = std::make_unique<PluginType>(std::forward<Args>(args)...);
        RegisterPlugin(std::move(plugin));
    }
    
    /**
     * @brief 安装所有插件
     * @param service 服务实例
     */
    bool InstallAll(Service& service);
    
    /**
     * @brief 卸载所有插件
     * @param service 服务实例
     */
    bool UninstallAll(Service& service);
    
    /**
     * @brief 获取插件数量
     */
    size_t GetPluginCount() const { return plugins_.size(); }
    
    /**
     * @brief 获取插件列表
     */
    const std::vector<std::unique_ptr<IPlugin>>& GetPlugins() const { return plugins_; }

private:
    std::vector<std::unique_ptr<IPlugin>> plugins_;
};

} // namespace service
} // namespace chwell
