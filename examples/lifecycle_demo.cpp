/**
 * @file lifecycle_demo.cpp
 * @brief 演示 ChwellCore 的 7 阶段生命周期
 * 
 * 编译:
 *   g++ -std=c++17 -I../include lifecycle_demo.cpp -L../build -lchwell_core -o lifecycle_demo
 * 
 * 运行:
 *   ./lifecycle_demo
 */

#include "chwell/service/service.h"
#include "chwell/service/component.h"
#include "chwell/core/logger.h"
#include <iostream>
#include <chrono>
#include <thread>

using namespace chwell::service;
using namespace chwell::net;

/**
 * @brief 示例组件：演示 7 阶段生命周期
 */
class DemoComponent : public Component {
public:
    DemoComponent(const std::string& name, int priority = 100)
        : name_(name), priority_(priority) {}
    
    std::string name() const override { return name_; }
    int priority() const override { return priority_; }
    
    // 阶段1: 初始化
    bool Init() override {
        std::cout << "[Init] " << name_ << ": 初始化内部状态" << std::endl;
        return true;
    }
    
    // 阶段2: 后初始化（建立依赖）
    bool PostInit() override {
        std::cout << "[PostInit] " << name_ << ": 建立依赖关系" << std::endl;
        return true;
    }
    
    // 阶段3: 检查配置
    bool CheckConfig() override {
        std::cout << "[CheckConfig] " << name_ << ": 检查配置" << std::endl;
        return true;
    }
    
    // 阶段4: 更新前准备
    bool PreUpdate() override {
        std::cout << "[PreUpdate] " << name_ << ": 准备更新循环" << std::endl;
        return true;
    }
    
    // 阶段5: 主更新循环
    bool Update(int64_t delta_ms) override {
        update_count_++;
        if (update_count_ <= 3) {
            std::cout << "[Update] " << name_ << ": 第 " << update_count_ 
                      << " 次更新 (delta=" << delta_ms << "ms)" << std::endl;
        }
        return true;
    }
    
    // 阶段6: 关闭前清理
    bool PreShut() override {
        std::cout << "[PreShut] " << name_ << ": 通知即将关闭" << std::endl;
        return true;
    }
    
    // 阶段7: 关闭
    bool Shut() override {
        std::cout << "[Shut] " << name_ << ": 释放资源" << std::endl;
        return true;
    }

private:
    std::string name_;
    int priority_;
    int update_count_{0};
};

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "ChwellCore 7 阶段生命周期演示" << std::endl;
    std::cout << "======================================" << std::endl;
    
    // 创建 Service（不监听网络，仅演示生命周期）
    Service service(9000, 1);
    
    std::cout << "\n[注册组件]" << std::endl;
    service.add_component<DemoComponent>("DatabaseComponent", 1);
    service.add_component<DemoComponent>("NetworkComponent", 2);
    service.add_component<DemoComponent>("GameComponent", 3);
    
    std::cout << "\n[启动 Service（触发生命周期）]" << std::endl;
    service.start();
    
    // 模拟几帧更新
    std::cout << "\n[模拟更新循环]" << std::endl;
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        service.Update();
    }
    
    std::cout << "\n[停止 Service]" << std::endl;
    service.stop();
    
    std::cout << "\n======================================" << std::endl;
    std::cout << "演示完成！" << std::endl;
    std::cout << "======================================" << std::endl;
    
    return 0;
}
