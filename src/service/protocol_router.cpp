#include "chwell/service/protocol_router.h"
#include "chwell/core/logger.h"
#include "chwell/protocol/message.h"

namespace chwell {
namespace service {

void ProtocolRouterComponent::on_message(const net::TcpConnectionPtr& conn,
                                         std::string_view data) {
    CHWELL_LOG_DEBUG("ProtocolRouter received " << data.size() << " bytes");

    // 在写锁下 feed 数据到解析器并获取结果
    std::vector<protocol::Message> messages;
    {
        std::unique_lock lock(parsers_mutex_);
        messages = parsers_[conn.get()].feed(data);
    }

    CHWELL_LOG_DEBUG("Parsed " << messages.size() << " message(s)");

    // 对每个解析出的消息进行路由（共享读锁访问 handlers）
    for (const auto& msg : messages) {
        CHWELL_LOG_DEBUG("Routing message cmd=0x" << std::hex << msg.cmd << std::dec);

        std::shared_lock hlock(handlers_mutex_);
        auto it = handlers_.find(msg.cmd);
        if (it != handlers_.end()) {
            CHWELL_LOG_DEBUG("Calling handler for cmd=0x" << std::hex << msg.cmd << std::dec);
            // 在读锁下调用 handler，避免死锁风险
            auto handler = it->second;
            hlock.unlock();
            handler(conn, msg);
        } else {
            CHWELL_LOG_WARN("No handler registered for cmd: 0x" << std::hex << msg.cmd << std::dec
                          << " (" << msg.cmd << ")");
        }
    }
}

void ProtocolRouterComponent::on_disconnect(const net::TcpConnectionPtr& conn) {
    CHWELL_LOG_DEBUG("ProtocolRouter cleanup for disconnected connection");
    std::unique_lock lock(parsers_mutex_);
    parsers_.erase(conn.get());
}

void ProtocolRouterComponent::send_message(const net::TcpConnectionPtr& conn,
                                           const protocol::Message& msg) {
    std::vector<char> data = protocol::serialize(msg);
    CHWELL_LOG_DEBUG("Sending message cmd=0x" << std::hex << msg.cmd << std::dec
                  << " size=" << data.size() << " bytes");
    conn->send(data);
}

} // namespace service
} // namespace chwell
