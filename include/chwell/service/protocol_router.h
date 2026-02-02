#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include "chwell/service/component.h"
#include "chwell/protocol/message.h"
#include "chwell/protocol/parser.h"

namespace chwell {
namespace net {
class TcpConnection;
typedef std::shared_ptr<TcpConnection> TcpConnectionPtr;
} // namespace net

namespace service {

// 协议路由组件：负责解析协议并按 cmd 路由到不同的处理器
// 使用方式：
//   1. 注册 ProtocolRouterComponent 到 Service
//   2. 调用 register_handler(cmd, handler) 注册各个 cmd 的处理器
//   3. 当收到消息时，会自动解析协议并按 cmd 路由
class ProtocolRouterComponent : public Component {
public:
    typedef std::function<void(const net::TcpConnectionPtr&, const protocol::Message&)> MessageHandler;

    ProtocolRouterComponent() {}

    virtual std::string name() const override {
        return "ProtocolRouterComponent";
    }

    // 注册一个 cmd 的处理器
    void register_handler(std::uint16_t cmd, MessageHandler handler) {
        handlers_[cmd] = handler;
    }

    // 组件接口：收到原始消息时，解析协议并路由
    virtual void on_message(const net::TcpConnectionPtr& conn,
                            const std::vector<char>& data) override;

    // 组件接口：连接断开时清理解析器
    virtual void on_disconnect(const net::TcpConnectionPtr& conn) override;

    // 发送协议消息的辅助函数
    static void send_message(const net::TcpConnectionPtr& conn, const protocol::Message& msg);

private:
    // 为每个连接维护一个解析器（处理粘包/拆包）
    std::unordered_map<const net::TcpConnection*, protocol::Parser> parsers_;
    std::unordered_map<std::uint16_t, MessageHandler> handlers_;
};

} // namespace service
} // namespace chwell
