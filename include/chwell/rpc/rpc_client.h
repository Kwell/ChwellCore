#pragma once

#include <string>
#include <functional>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <thread>

#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/protocol/message.h"
#include "chwell/protocol/parser.h"

namespace chwell {

namespace circuitbreaker { class CircuitBreaker; }
namespace ratelimit { class RateLimiter; }

namespace rpc {

typedef std::function<void(bool ok, const protocol::Message& response)> RpcCallback;

// RPC 调用的内部请求记录
struct PendingRequest {
    RpcCallback callback;
    std::chrono::steady_clock::time_point deadline;
};

class RpcClient {
public:
    explicit RpcClient(net::IoService& io_service, int default_timeout_seconds = 5)
        : io_service_(io_service)
        , connection_()
        , next_request_id_(1)
        , default_timeout_seconds_(default_timeout_seconds)
        , cleanup_running_(false) {}

    ~RpcClient();

    bool connect(const std::string& host, unsigned short port);

    // 异步调用：callback(ok, response)，ok=false 表示超时或连接断开
    void call(std::uint16_t cmd, const std::vector<char>& request_data,
              RpcCallback callback, int timeout_seconds = -1);

    // 同步调用，阻塞直到收到响应或超时，返回是否成功
    bool call_sync(std::uint16_t cmd, const std::vector<char>& request_data,
                   protocol::Message& response, int timeout_seconds = 5);

    void disconnect();

    bool is_connected() const { return connection_ != nullptr; }

    // 设置熔断器（可选）
    void set_circuit_breaker(std::shared_ptr<circuitbreaker::CircuitBreaker> cb) {
        circuit_breaker_ = cb;
    }

    // 设置限流器（可选）
    void set_rate_limiter(std::shared_ptr<ratelimit::RateLimiter> rl) {
        rate_limiter_ = rl;
    }

private:
    void on_message(const net::TcpConnectionPtr& conn, const std::vector<char>& data);
    void on_disconnect(const net::TcpConnectionPtr& conn);

    // 启动/停止超时清理线程
    void start_cleanup_thread();
    void stop_cleanup_thread();
    void cleanup_expired_requests();

    net::IoService& io_service_;
    net::TcpConnectionPtr connection_;
    std::atomic<std::uint32_t> next_request_id_;
    int default_timeout_seconds_;

    mutable std::mutex requests_mutex_;
    std::unordered_map<std::uint32_t, PendingRequest> pending_requests_;

    // 超时清理线程
    std::thread cleanup_thread_;
    std::atomic<bool> cleanup_running_;
    std::mutex cleanup_mutex_;
    std::condition_variable cleanup_cv_;

    // Receive buffer for handling multiple messages per TCP read
    std::vector<char> recv_buffer_;

    std::shared_ptr<circuitbreaker::CircuitBreaker> circuit_breaker_;
    std::shared_ptr<ratelimit::RateLimiter> rate_limiter_;
};

} // namespace rpc
} // namespace chwell
