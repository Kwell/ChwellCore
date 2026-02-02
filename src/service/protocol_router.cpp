#include "chwell/service/protocol_router.h"
#include "chwell/core/logger.h"
#include "chwell/protocol/message.h"

namespace chwell {
namespace service {

void ProtocolRouterComponent::on_message(const net::TcpConnectionPtr& conn,
                                         const std::vector<char>& data) {
    // 获取或创建该连接的解析器
    auto& parser = parsers_[conn.get()];

    // 解析消息
    std::vector<protocol::Message> messages = parser.feed(data);

    // 对每个解析出的消息进行路由
    for (const auto& msg : messages) {
        auto it = handlers_.find(msg.cmd);
        if (it != handlers_.end()) {
            // 找到对应的处理器，调用它
            it->second(conn, msg);
        } else {
            // 没有注册的处理器，记录警告
            core::Logger::instance().warn("No handler registered for cmd: " + std::to_string(msg.cmd));
        }
    }
}

void ProtocolRouterComponent::on_disconnect(const net::TcpConnectionPtr& conn) {
    // 清理该连接的解析器
    parsers_.erase(conn.get());
}

void ProtocolRouterComponent::send_message(const net::TcpConnectionPtr& conn,
                                           const protocol::Message& msg) {
    std::vector<char> data = protocol::serialize(msg);
    conn->send(data);
}

} // namespace service
} // namespace chwell
