#pragma once

#include <unordered_map>
#include <chrono>
#include <mutex>

namespace chwell {
namespace reliability {

// 简单的令牌桶限流器
class RateLimiter {
public:
    RateLimiter(int max_requests_per_second, int burst_size = 10)
        : max_rate_(max_requests_per_second),
          burst_size_(burst_size),
          tokens_(burst_size),
          last_update_(std::chrono::steady_clock::now()) {}

    // 检查是否允许请求
    bool allow() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_update_).count();

        // 补充令牌
        int tokens_to_add = static_cast<int>(elapsed * max_rate_ / 1000);
        tokens_ = std::min(burst_size_, tokens_ + tokens_to_add);
        last_update_ = now;

        if (tokens_ > 0) {
            --tokens_;
            return true;
        }
        return false;
    }

private:
    int max_rate_;
    int burst_size_;
    int tokens_;
    std::chrono::steady_clock::time_point last_update_;
    std::mutex mutex_;
};

// 按连接限流
class ConnectionRateLimiter {
public:
    ConnectionRateLimiter(int max_requests_per_second, int burst_size = 10)
        : max_rate_(max_requests_per_second), burst_size_(burst_size) {}

    bool allow(const void* conn_id) {
        auto it = limiters_.find(conn_id);
        if (it == limiters_.end()) {
            it = limiters_.emplace(conn_id, RateLimiter(max_rate_, burst_size_)).first;
        }
        return it->second.allow();
    }

private:
    int max_rate_;
    int burst_size_;
    std::unordered_map<const void*, RateLimiter> limiters_;
};

} // namespace reliability
} // namespace chwell
