#pragma once

#include <string>
#include <vector>

#include "chwell/net/tcp_connection.h"

namespace chwell {
namespace service {

class Service; // 前向声明

// 组件基类：下游只需要继承该类并实现若干虚函数，即可挂到 Service 上
class Component {
public:
    virtual ~Component() {}

    // 组件名称（用于日志、调试）
    virtual std::string name() const = 0;

    // 组件被注册到 Service 时调用，可在此做初始化或向 Service 注册路由等
    virtual void on_register(Service& /*svc*/) {}

    // 收到一条来自某连接的原始消息时的回调
    // 这里先用最简单的字节流接口，下游可以在内部自行做协议解析
    virtual void on_message(const net::TcpConnectionPtr& /*conn*/,
                            const std::vector<char>& /*data*/) {}

    // 连接断开时的回调（可选实现）
    virtual void on_disconnect(const net::TcpConnectionPtr& /*conn*/) {}
};

} // namespace service
} // namespace chwell

