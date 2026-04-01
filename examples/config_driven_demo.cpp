/**
 * @file config_driven_demo.cpp
 * @brief 演示 ChwellCore 的配置驱动功能
 * 
 * 编译:
 *   g++ -std=c++17 -I../include config_driven_demo.cpp -L../build -lchwell_core -o config_driven_demo
 * 
 * 运行:
 *   ./config_driven_demo
 */

#include "chwell/service/service.h"
#include "chwell/service/component.h"
#include "chwell/core/config.h"
#include "chwell/core/logger.h"
#include <iostream>
#include <memory>

using namespace chwell::service;
using namespace chwell::core;
using namespace chwell::net;

/**
 * @brief 可配置的组件：根据配置决定是否启用和优先级
 */
class ConfigurableComponent : public Component {
public:
    ConfigurableComponent(const std::string& name, const Config& config)
        : name_(name), config_(config) {
        // 从配置读取启用状态和优先级
        enabled_ = config_.is_component_enabled(name_);
        priority_ = config_.get_component_priority(name_);
    }
    
    std::string name() const override { return name_; }
    int priority() const override { return priority_; }
    
    bool Init() override {
        if (!enabled_) {
            std::cout << "[Init] " << name_ << ": 已禁用，跳过初始化" << std::endl;
            return true;
        }
        
        std::cout << "[Init] " << name_ << ": 初始化 (priority=" << priority_ << ")" << std::endl;
        
        // 读取组件参数
        for (const auto& comp : config_.components()) {
            if (comp.name == name_) {
                for (const auto& param : comp.params) {
                    std::cout << "  参数 " << param.first << " = " << param.second << std::endl;
                }
            }
        }
        
        return true;
    }
    
    bool PostInit() override {
        if (!enabled_) return true;
        std::cout << "[PostInit] " << name_ << ": 建立依赖" << std::endl;
        return true;
    }
    
    bool Update(int64_t delta_ms) override {
        return true;
    }
    
    bool Shut() override {
        if (!enabled_) return true;
        std::cout << "[Shut] " << name_ << ": 关闭" << std::endl;
        return true;
    }

private:
    std::string name_;
    const Config& config_;
    bool enabled_;
    int priority_;
};

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "ChwellCore 配置驱动演示" << std::endl;
    std::cout << "======================================" << std::endl;
    
    // 加载配置
    Config config;
    if (!config.load_from_file("../config/game_server.conf")) {
        std::cerr << "加载配置失败！" << std::endl;
        return 1;
    }
    
    std::cout << "\n[服务配置]" << std::endl;
    std::cout << "  server_name: " << config.server_name() << std::endl;
    std::cout << "  bus_id: " << config.bus_id() << std::endl;
    std::cout << "  listen_port: " << config.listen_port() << std::endl;
    std::cout << "  worker_threads: " << config.worker_threads() << std::endl;
    
    std::cout << "\n[组件配置]" << std::endl;
    for (const auto& comp : config.components()) {
        std::cout << "  " << comp.name << ": enabled=" << comp.enabled 
                  << ", priority=" << comp.priority << std::endl;
    }
    
    // 创建 Service
    Service service(config.listen_port(), config.worker_threads());
    
    std::cout << "\n[注册组件]" << std::endl;
    service.add_component<ConfigurableComponent>("ProtocolRouter", config);
    service.add_component<ConfigurableComponent>("SessionManager", config);
    service.add_component<ConfigurableComponent>("PlayerManager", config);
    service.add_component<ConfigurableComponent>("BattleManager", config);
    service.add_component<ConfigurableComponent>("RankManager", config);
    service.add_component<ConfigurableComponent>("Database", config);
    service.add_component<ConfigurableComponent>("Redis", config);
    
    std::cout << "\n[启动 Service]" << std::endl;
    service.start();
    
    std::cout << "\n[停止 Service]" << std::endl;
    service.stop();
    
    std::cout << "\n======================================" << std::endl;
    std::cout << "演示完成！" << std::endl;
    std::cout << "======================================" << std::endl;
    
    return 0;
}
