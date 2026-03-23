#include "chwell/rpc/rpc_client.h"
#include "chwell/core/logger.h"
#include "chwell/protocol/message.h"
#include "chwell/protocol/parser.h"
#include "chwell/circuitbreaker/circuit_breaker.h"
#include "chwell/ratelimit/rate_limiter.h"
#include <cstring>
#include <arpa/inet.h>
#include <stdexcept>

namespace chwell {
namespace rpc {

// RPC body prefix: first 4 bytes are request_id (big-endian).
// Server echoes back the same 4-byte prefix in the response body.

static std::vector<char> encode_rpc_body(std::uint32_t request_id,
                                          const std::vector<char>& payload) {
    std::vector<char> body(4 + payload.size());
    std::uint32_t id_be = htonl(request_id);
    memcpy(body.data(), &id_be, 4);
    if (!payload.empty()) {
        memcpy(body.data() + 4, payload.data(), payload.size());
    }
    return body;
}

static bool decode_rpc_body(const std::vector<char>& body,
                              std::uint32_t& request_id,
                              std::vector<char>& payload) {
    if (body.size() < 4) {
        return false;
    }
    std::uint32_t id_be;
    memcpy(&id_be, body.data(), 4);
    request_id = ntohl(id_be);
    payload.assign(body.begin() + 4, body.end());
    return true;
}

RpcClient::~RpcClient() {
    disconnect();
}

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
        CHWELL_LOG_ERROR("RPC connect: invalid address " << host);
        close(fd);
        return false;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        CHWELL_LOG_ERROR("RPC connect failed: " << strerror(errno));
        close(fd);
        return false;
    }

    net::TcpSocket tcpsocket(fd);
    connection_ = std::make_shared<net::TcpConnection>(std::move(tcpsocket));
    connection_->set_message_callback([this](const net::TcpConnectionPtr& conn,
                                             const std::vector<char>& data) {
        on_message(conn, data);
    });
    connection_->set_close_callback([this](const net::TcpConnectionPtr& conn) {
        on_disconnect(conn);
    });

    net::TcpConnectionPtr conn_ptr = connection_;
    io_service_.post([conn_ptr]() { conn_ptr->start(); });

    start_cleanup_thread();

    CHWELL_LOG_INFO("RPC connected to " << host << ":" << port);
    return true;
}

void RpcClient::disconnect() {
    stop_cleanup_thread();
    if (connection_) {
        connection_->close();
        connection_.reset();
    }
    // Notify all pending requests of failure
    std::unordered_map<std::uint32_t, PendingRequest> pending;
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        pending.swap(pending_requests_);
    }
    protocol::Message empty_msg;
    for (auto& kv : pending) {
        if (kv.second.callback) {
            kv.second.callback(false, empty_msg);
        }
    }
}

void RpcClient::call(std::uint16_t cmd, const std::vector<char>& request_data,
                     RpcCallback callback, int timeout_seconds) {
    if (!connection_) {
        CHWELL_LOG_ERROR("RPC call failed: not connected");
        if (callback) {
            protocol::Message empty;
            callback(false, empty);
        }
        return;
    }

    // Circuit breaker check
    if (circuit_breaker_) {
        auto state = circuit_breaker_->get_state();
        if (state == circuitbreaker::CircuitState::OPEN) {
            CHWELL_LOG_WARN("RPC call rejected by circuit breaker (OPEN)");
            if (callback) {
                protocol::Message empty;
                callback(false, empty);
            }
            return;
        }
    }

    // Rate limiter check
    if (rate_limiter_) {
        auto result = rate_limiter_->check("rpc");
        if (!result.allowed) {
            CHWELL_LOG_WARN("RPC call rejected by rate limiter");
            if (callback) {
                protocol::Message empty;
                callback(false, empty);
            }
            return;
        }
        rate_limiter_->consume("rpc");
    }

    std::uint32_t request_id = next_request_id_.fetch_add(1);
    int effective_timeout = (timeout_seconds < 0) ? default_timeout_seconds_ : timeout_seconds;

    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        PendingRequest req;
        req.callback = callback;
        req.deadline = std::chrono::steady_clock::now()
                       + std::chrono::seconds(effective_timeout);
        pending_requests_[request_id] = std::move(req);
    }

    std::vector<char> body = encode_rpc_body(request_id, request_data);
    protocol::Message msg(cmd, body);
    std::vector<char> data = protocol::serialize(msg);
    connection_->send(data);
}

bool RpcClient::call_sync(std::uint16_t cmd, const std::vector<char>& request_data,
                          protocol::Message& response, int timeout_seconds) {
    if (!connection_) {
        CHWELL_LOG_ERROR("RPC call_sync failed: not connected");
        return false;
    }

    std::mutex sync_mutex;
    std::condition_variable sync_cv;
    bool done = false;
    bool success = false;

    call(cmd, request_data,
         [&](bool ok, const protocol::Message& resp) {
             std::lock_guard<std::mutex> lock(sync_mutex);
             success = ok;
             response = resp;
             done = true;
             sync_cv.notify_one();
         },
         timeout_seconds);

    std::unique_lock<std::mutex> lock(sync_mutex);
    bool signaled = sync_cv.wait_for(lock,
                                      std::chrono::seconds(timeout_seconds + 1),
                                      [&done]() { return done; });
    if (!signaled) {
        CHWELL_LOG_WARN("RPC call_sync timed out waiting for response");
        return false;
    }
    return success;
}

void RpcClient::on_message(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    (void)conn;
    // Accumulate and parse multiple messages per read (handle batched TCP delivery)
    recv_buffer_.insert(recv_buffer_.end(), data.begin(), data.end());

    protocol::Parser parser;
    auto messages = parser.feed(recv_buffer_);

    if (!messages.empty()) {
        // Remove consumed bytes from buffer
        size_t consumed = 0;
        for (const auto& m : messages) {
            consumed += 4 + m.body.size(); // cmd(2) + len(2) + body
        }
        if (consumed <= recv_buffer_.size()) {
            recv_buffer_.erase(recv_buffer_.begin(), recv_buffer_.begin() + consumed);
        }
    }

    for (const auto& msg : messages) {
        std::uint32_t request_id = 0;
        std::vector<char> payload;
        if (!decode_rpc_body(msg.body, request_id, payload)) {
            CHWELL_LOG_WARN("RPC on_message: malformed response (missing request_id)");
            continue;
        }

        PendingRequest req;
        {
            std::lock_guard<std::mutex> lock(requests_mutex_);
            auto it = pending_requests_.find(request_id);
            if (it == pending_requests_.end()) {
                CHWELL_LOG_WARN("RPC on_message: unknown request_id=" << request_id);
                continue;
            }
            req = std::move(it->second);
            pending_requests_.erase(it);
        }

        protocol::Message real_response(msg.cmd, payload);
        if (req.callback) {
            req.callback(true, real_response);
        }
    }
}

void RpcClient::on_disconnect(const net::TcpConnectionPtr& conn) {
    (void)conn;
    CHWELL_LOG_WARN("RPC: backend disconnected, failing all pending requests");
    std::unordered_map<std::uint32_t, PendingRequest> pending;
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        pending.swap(pending_requests_);
    }
    protocol::Message empty_msg;
    for (auto& kv : pending) {
        if (kv.second.callback) {
            kv.second.callback(false, empty_msg);
        }
    }
    connection_.reset();
}

void RpcClient::start_cleanup_thread() {
    cleanup_running_.store(true);
    cleanup_thread_ = std::thread([this]() {
        while (cleanup_running_.load()) {
            std::unique_lock<std::mutex> lock(cleanup_mutex_);
            cleanup_cv_.wait_for(lock, std::chrono::seconds(1),
                                 [this]() { return !cleanup_running_.load(); });
            if (!cleanup_running_.load()) break;
            cleanup_expired_requests();
        }
    });
}

void RpcClient::stop_cleanup_thread() {
    cleanup_running_.store(false);
    cleanup_cv_.notify_all();
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
}

void RpcClient::cleanup_expired_requests() {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<std::uint32_t, PendingRequest>> expired;
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        for (auto it = pending_requests_.begin(); it != pending_requests_.end(); ) {
            if (now >= it->second.deadline) {
                expired.emplace_back(it->first, std::move(it->second));
                it = pending_requests_.erase(it);
            } else {
                ++it;
            }
        }
    }

    protocol::Message empty_msg;
    for (auto& kv : expired) {
        CHWELL_LOG_WARN("RPC request timed out, id=" << kv.first);
        // Report failure to circuit breaker
        if (circuit_breaker_) {
            bool threw = false;
            try {
                circuit_breaker_->execute([&threw]() {
                    threw = true;
                    throw std::runtime_error("rpc timeout");
                });
            } catch (...) {}
        }
        if (kv.second.callback) {
            kv.second.callback(false, empty_msg);
        }
    }
}

} // namespace rpc
} // namespace chwell
