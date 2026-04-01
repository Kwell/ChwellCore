#pragma once

#include <string>
#include <string_view>
#include <cstdint>

#include "chwell/net/tcp_connection.h"

namespace chwell {
namespace service {

class Service; // 前向声明

/**
 * @brief 组件基类：支持 7 阶段生命周期
 * 
 * 生命周期顺序：
 * 1. Init() - 初始化组件内部状态
 * 2. PostInit() - 依赖注入完成，建立组件间关系
 * 3. CheckConfig() - 检查配置正确性
 * 4. PreUpdate() - 更新前准备（启动定时器等）
 * 5. Update() - 主更新循环（每帧调用）
 * 6. PreShut() - 关闭前清理（通知其他组件）
 * 7. Shut() - 关闭（释放资源）
 */
class Component {
public:
    virtual ~Component() {}

    // ========== 组件基础信息 ==========
    
    // 组件名称（用于日志、调试）
    virtual std::string name() const = 0;
    
    // 组件优先级（用于初始化顺序，数字越小越先初始化）
    virtual int priority() const { return 100; }

    // ========== 7 阶段生命周期 ==========
    
    // 阶段1: 初始化（组件内部初始化）
    virtual bool Init() { return true; }
    
    // 阶段2: 后初始化（所有组件 Init 完成后调用，用于建立依赖关系）
    virtual bool PostInit() { return true; }
    
    // 阶段3: 检查配置（验证配置正确性）
    virtual bool CheckConfig() { return true; }
    
    // 阶段4: 更新前准备（启动定时器、开始监听等）
    virtual bool PreUpdate() { return true; }
    
    // 阶段5: 主更新循环（每帧调用，delta_ms 为时间间隔）
    virtual bool Update(int64_t delta_ms) { return true; }
    
    // 阶段6: 关闭前清理（通知其他组件即将关闭）
    virtual bool PreShut() { return true; }
    
    // 阶段7: 关闭（释放资源）
    virtual bool Shut() { return true; }

    // ========== 消息回调（向后兼容）==========
    
    // 兼容旧接口：组件被注册到 Service 时调用（已废弃，使用 Init 替代）
    virtual void on_register(Service& /*svc*/) {}

    // 收到一条来自某连接的原始消息时的回调
    // data 仅在回调返回前有效（与 TcpConnection 读缓冲一致）
    virtual void on_message(const net::TcpConnectionPtr& /*conn*/,
                            std::string_view /*data*/) {}

    // 连接断开时的回调（可选实现）
    virtual void on_disconnect(const net::TcpConnectionPtr& /*conn*/) {}
};

} // namespace service
} // namespace chwell

