#include "chwell/rpc/rpc_client.h"
#include "chwell/core/logger.h"
#include "chwell/protocol/message.h"
#include <asio.hpp>

namespace chwell {
namespace rpc {

bool RpcClient::connect(const std::string& host, unsigned short port) {
    try {
        asio::ip::tcp::resolver resolver(io_service_);
        asio::ip::tcp::resolver::query query(host, std::to_string(port));
        asio::ip::tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);

        asio::ip::tcp::socket socket(io_service_);
        asio::connect(socket, endpoint_iterator);

        connection_ = std::make_shared<net::TcpConnection>(std::move(socket));
        connection_->set_message_callback([this](const net::TcpConnectionPtr& conn,
                                                   const std::vector<char>& data) {
            on_message(conn, data);
        });
        connection_->start();

        return true;
    } catch (const std::exception& e) {
        core::Logger::instance().error("RPC connect failed: " + std::string(e.what()));
        return false;
    }
}

void RpcClient::call(std::uint16_t cmd, const std::vector<char>& request_data, RpcCallback callback) {
    if (!connection_) {
        core::Logger::instance().error("RPC call failed: not connected");
        return;
    }

    std::uint32_t request_id = next_request_id_++;
    pending_requests_[request_id] = callback;

    // 简化实现：将request_id编码到body前4字节
    protocol::Message msg(cmd, request_data);
    std::vector<char> data = protocol::serialize(msg);
    connection_->send(data);
}

void RpcClient::on_message(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    protocol::Message msg;
    if (protocol::deserialize(data, msg)) {
        // 简化实现：假设响应包含request_id
        // 实际应该从消息中解析request_id并查找对应的callback
        // 这里先简化处理
        if (!pending_requests_.empty()) {
            auto it = pending_requests_.begin();
            it->second(msg);
            pending_requests_.erase(it);
        }
    }
}

bool RpcClient::call_sync(std::uint16_t cmd, const std::vector<char>& request_data,
                          protocol::Message& response, int timeout_seconds) {
    // 简化实现：同步调用需要更复杂的实现（等待响应）
    // 这里先返回false表示未实现
    core::Logger::instance().warn("RPC sync call not fully implemented");
    return false;
}

} // namespace rpc
} // namespace chwell
