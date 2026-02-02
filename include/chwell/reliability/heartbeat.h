#pragma once

#include <unordered_map>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>

#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace reliability {

// 心跳管理器：定期检测超时连接（使用 std::thread 替代 asio::steady_timer）
class HeartbeatManager {
public:
    HeartbeatManager(IoService& io_service, int heartbeat_interval_seconds = 30)
        : io_service_(io_service),
          heartbeat_interval_(heartbeat_interval_seconds),
          stopped_(false) {}

    ~HeartbeatManager() {
        stopped_ = true;
        if (timer_thread_.joinable()) {
            timer_thread_.join();
        }
    }

    void register_connection(const net::TcpConnectionPtr& conn) {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        connections_[conn.get()] = now;
        if (!timer_thread_.joinable()) {
            timer_thread_ = std::thread([this]() { timer_loop(); });
        }
    }

    void unregister_connection(const net::TcpConnectionPtr& conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(conn.get());
    }

    void update_active_time(const net::TcpConnectionPtr& conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(conn.get());
        if (it != connections_.end()) {
            it->second = std::chrono::steady_clock::now();
        }
    }

private:
    void timer_loop() {
        while (!stopped_) {
            std::this_thread::sleep_for(std::chrono::seconds(heartbeat_interval_));
            if (stopped_) break;
            check_connections();
        }
    }

    void check_connections() {
        auto now = std::chrono::steady_clock::now();
        auto timeout = std::chrono::seconds(heartbeat_interval_ * 3);

        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            auto elapsed = now - it->second;
            if (elapsed > timeout) {
                core::Logger::instance().warn("Connection timeout detected");
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }

    IoService& io_service_;
    int heartbeat_interval_;
    std::atomic<bool> stopped_;
    std::thread timer_thread_;
    std::mutex mutex_;
    std::unordered_map<const net::TcpConnection*, std::chrono::steady_clock::time_point> connections_;
};

} // namespace reliability
} // namespace chwell
