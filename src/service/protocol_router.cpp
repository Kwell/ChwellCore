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

        // 为连接分配唯一 ID（首次见到该连接时）
        // 使用 ID 而非裸指针作为 key，避免指针地址复用导致新连接
        // 错误继承旧连接的解析器状态（半包残留等）
        auto& conn_id = conn_ids_[conn->conn_id()];
        if (conn_id == 0) {
            conn_id = next_conn_id_.fetch_add(1, std::memory_order_relaxed);
        }

        messages = parsers_[conn_id].feed(data);
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

    // 通过连接指针找到 ID，再清理解析器和 ID 映射
    auto id_it = conn_ids_.find(conn->conn_id());
    if (id_it != conn_ids_.end()) {
        uint64_t parser_slot = id_it->second;
        parsers_.erase(parser_slot);
        conn_ids_.erase(id_it);
    }
}

void ProtocolRouterComponent::send_message(const net::TcpConnectionPtr& conn,
                                           const protocol::Message& msg) {
    std::vector<char> data = protocol::serialize(msg);
    if (data.empty()) {
        // serialize 拒绝超长 body（>65535），避免发出截断帧破坏流
        CHWELL_LOG_ERROR("send_message dropped: serialize failed (body too large?) cmd=0x"
                         << std::hex << msg.cmd << std::dec);
        return;
    }
    CHWELL_LOG_DEBUG("Sending message cmd=0x" << std::hex << msg.cmd << std::dec
                  << " size=" << data.size() << " bytes");
    conn->send(data);
}

} // namespace service
} // namespace chwell
