#pragma once

#include <string>
#include <functional>
#include <memory>
#include <unordered_map>

#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/protocol/message.h"

namespace chwell {
namespace rpc {

typedef std::function<void(const protocol::Message& response)> RpcCallback;

class RpcClient {
public:
    RpcClient(net::IoService& io_service)
        : io_service_(io_service), connection_(), next_request_id_(1) {}

    bool connect(const std::string& host, unsigned short port);

    void call(std::uint16_t cmd, const std::vector<char>& request_data, RpcCallback callback);

    bool call_sync(std::uint16_t cmd, const std::vector<char>& request_data,
                   protocol::Message& response, int timeout_seconds = 5);

    void disconnect() {
        if (connection_) {
            connection_->close();
            connection_.reset();
        }
    }

private:
    void on_message(const net::TcpConnectionPtr& conn, const std::vector<char>& data);

    net::IoService& io_service_;
    net::TcpConnectionPtr connection_;
    std::uint32_t next_request_id_;
    std::unordered_map<std::uint32_t, RpcCallback> pending_requests_;
};

} // namespace rpc
} // namespace chwell
