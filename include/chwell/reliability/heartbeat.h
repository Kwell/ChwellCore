#pragma once

#include <asio.hpp>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include "chwell/net/tcp_connection.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace reliability {

// 心跳管理器：定期发送心跳并检测超时连接
class HeartbeatManager {
public:
    HeartbeatManager(asio::io_service& io_service, int heartbeat_interval_seconds = 30)
        : io_service_(io_service),
          heartbeat_interval_(heartbeat_interval_seconds),
          timer_(io_service_) {}

    // 注册连接，开始心跳检测
    void register_connection(const net::TcpConnectionPtr& conn) {
        auto now = std::chrono::steady_clock::now();
        connections_[conn.get()] = now;
        start_timer();
    }

    // 注销连接
    void unregister_connection(const net::TcpConnectionPtr& conn) {
        connections_.erase(conn.get());
    }

    // 更新连接的最后活跃时间
    void update_active_time(const net::TcpConnectionPtr& conn) {
        auto it = connections_.find(conn.get());
        if (it != connections_.end()) {
            it->second = std::chrono::steady_clock::now();
        }
    }

private:
    void start_timer() {
        timer_.expires_from_now(std::chrono::seconds(heartbeat_interval_));
        timer_.async_wait([this](const asio::error_code& ec) {
            if (!ec) {
                check_connections();
                start_timer();
            }
        });
    }

    void check_connections() {
        auto now = std::chrono::steady_clock::now();
        auto timeout = std::chrono::seconds(heartbeat_interval_ * 3); // 3倍间隔超时

        for (auto it = connections_.begin(); it != connections_.end();) {
            auto elapsed = now - it->second;
            if (elapsed > timeout) {
                core::Logger::instance().warn("Connection timeout detected");
                // TODO: 触发超时回调或关闭连接
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }

    asio::io_service& io_service_;
    int heartbeat_interval_;
    asio::steady_timer timer_;
    std::unordered_map<const net::TcpConnection*, std::chrono::steady_clock::time_point> connections_;
};

} // namespace reliability
} // namespace chwell
