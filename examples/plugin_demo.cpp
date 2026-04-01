/**
 * @file plugin_demo.cpp
 * @brief 演示 ChwellCore 的插件系统
 * 
 * 编译:
 *   g++ -std=c++17 -I../include plugin_demo.cpp -L../build -lchwell_core -o plugin_demo
 * 
 * 运行:
 *   ./plugin_demo
 */

#include "chwell/service/service.h"
#include "chwell/service/component.h"
#include "chwell/service/plugin.h"
#include "chwell/core/logger.h"
#include <iostream>

using namespace chwell::service;
using namespace chwell::net;

// ========== 示例组件 ==========

class NetworkComponent : public Component {
public:
    std::string name() const override { return "NetworkComponent"; }
    int priority() const override { return 1; }
    
    bool Init() override {
        std::cout << "[NetworkComponent] Init: 初始化网络层" << std::endl;
        return true;
    }
};

class DatabaseComponent : public Component {
public:
    std::string name() const override { return "DatabaseComponent"; }
    int priority() const override { return 2; }
    
    bool Init() override {
        std::cout << "[DatabaseComponent] Init: 连接数据库" << std::endl;
        return true;
    }
};

class GameComponent : public Component {
public:
    std::string name() const override { return "GameComponent"; }
    int priority() const override { return 3; }
    
    bool Init() override {
        std::cout << "[GameComponent] Init: 初始化游戏逻辑" << std::endl;
        return true;
    }
};

// ========== 示例插件 ==========

/**
 * @brief 基础设施插件：管理网络和数据库组件
 */
class InfrastructurePlugin : public IPlugin {
public:
    const std::string& GetName() const override {
        static std::string name = "InfrastructurePlugin";
        return name;
    }
    
    int GetVersion() const override { return 1; }
    int GetPriority() const override { return 1; } // 优先加载
    
    bool Install(Service& service) override {
        std::cout << "[InfrastructurePlugin] Installing..." << std::endl;
        
        // 注册组件
        service.add_component<NetworkComponent>();
        service.add_component<DatabaseComponent>();
        
        std::cout << "[InfrastructurePlugin] Installed 2 components" << std::endl;
        return true;
    }
    
    bool Uninstall(Service& service) override {
        std::cout << "[InfrastructurePlugin] Uninstalling..." << std::endl;
        return true;
    }
};

/**
 * @brief 游戏逻辑插件：管理游戏组件
 */
class GameLogicPlugin : public IPlugin {
public:
    const std::string& GetName() const override {
        static std::string name = "GameLogicPlugin";
        return name;
    }
    
    int GetVersion() const override { return 1; }
    int GetPriority() const override { return 2; } // 第二加载
    
    bool Install(Service& service) override {
        std::cout << "[GameLogicPlugin] Installing..." << std::endl;
        
        // 注册组件
        service.add_component<GameComponent>();
        
        std::cout << "[GameLogicPlugin] Installed 1 component" << std::endl;
        return true;
    }
    
    bool Uninstall(Service& service) override {
        std::cout << "[GameLogicPlugin] Uninstalling..." << std::endl;
        return true;
    }
};

// ========== 主程序 ==========

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "ChwellCore 插件系统演示" << std::endl;
    std::cout << "======================================" << std::endl;
    
    // 创建 Service
    Service service(9000, 1);
    
    std::cout << "\n[注册插件]" << std::endl;
    auto& plugin_manager = service.plugin_manager();
    
    // 注册插件（注意：GameLogicPlugin 优先级=2，会在 InfrastructurePlugin 优先级=1 之后加载）
    plugin_manager.RegisterPlugin<InfrastructurePlugin>();
    plugin_manager.RegisterPlugin<GameLogicPlugin>();
    
    std::cout << "已注册 " << plugin_manager.GetPluginCount() << " 个插件" << std::endl;
    
    // 安装插件（会注册组件）
    std::cout << "\n[安装插件]" << std::endl;
    plugin_manager.InstallAll(service);
    
    // 启动 Service
    std::cout << "\n[启动 Service]" << std::endl;
    service.start();
    
    // 停止 Service
    std::cout << "\n[停止 Service]" << std::endl;
    service.stop();
    
    std::cout << "\n======================================" << std::endl;
    std::cout << "演示完成！" << std::endl;
    std::cout << "======================================" << std::endl;
    
    return 0;
}
