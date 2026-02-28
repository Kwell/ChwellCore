#include "chwell/rpc/rpc_client.h"
#include "chwell/core/logger.h"
#include "chwell/protocol/message.h"
#include <cstring>

namespace chwell {
namespace rpc {

bool RpcClient::connect(const std::string& host, unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        CHWELL_LOG_ERROR("RPC connect: socket failed");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        CHWELL_LOG_ERROR("RPC connect: invalid address");
        close(fd);
        return false;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        CHWELL_LOG_ERROR("RPC connect failed: " + std::string(strerror(errno)));
        close(fd);
        return false;
    }

    net::TcpSocket socket(fd);
    connection_ = std::make_shared<net::TcpConnection>(std::move(socket));
    connection_->set_message_callback([this](const net::TcpConnectionPtr& conn,
                                             const std::vector<char>& data) {
        on_message(conn, data);
    });
    net::TcpConnectionPtr conn = connection_;
    io_service_.post([conn]() {
        conn->start();
    });

    CHWELL_LOG_INFO("RPC connected to " << host << ":" << port);
    return true;
}

void RpcClient::call(std::uint16_t cmd, const std::vector<char>& request_data, RpcCallback callback) {
    if (!connection_) {
        CHWELL_LOG_ERROR("RPC call failed: not connected");
        return;
    }

    std::uint32_t request_id = next_request_id_++;
    pending_requests_[request_id] = callback;

    protocol::Message msg(cmd, request_data);
    std::vector<char> data = protocol::serialize(msg);
    connection_->send(data);
}

void RpcClient::on_message(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    (void)conn;
    protocol::Message msg;
    if (protocol::deserialize(data, msg)) {
        if (!pending_requests_.empty()) {
            auto it = pending_requests_.begin();
            it->second(msg);
            pending_requests_.erase(it);
        }
    }
}

bool RpcClient::call_sync(std::uint16_t cmd, const std::vector<char>& request_data,
                          protocol::Message& response, int timeout_seconds) {
    (void)cmd;
    (void)request_data;
    (void)response;
    (void)timeout_seconds;
    CHWELL_LOG_WARN("RPC sync call not fully implemented");
    return false;
}

} // namespace rpc
} // namespace chwell
