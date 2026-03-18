#pragma once

#include "chwell/core/logger.h"
#include <string>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <queue>
#include <chrono>
#include <thread>

namespace chwell {
namespace ratelimit {

// 限流策略
enum class RateLimitStrategy {
    TOKEN_BUCKET,        // 令牌桶
    LEAKY_BUCKET,       // 漏桶
    SLIDING_WINDOW_LOG,  // 滑动窗口日志
    FIXED_WINDOW,        // 固定窗口
};

// 限流结果
struct RateLimitResult {
    bool allowed;           // 是否允许
    int64_t remaining;     // 剩余配额
    int64_t wait_time_ms;  // 需要等待时间（毫秒）
    std::string reason;     // 拒绝原因

    RateLimitResult()
        : allowed(true), remaining(0), wait_time_ms(0) {}
};

// ============================================
// 限流器接口
// ============================================

class RateLimiter {
public:
    virtual ~RateLimiter() = default;

    // 检查是否允许
    virtual RateLimitResult check(const std::string& key) = 0;

    // 消耗配额
    virtual bool consume(const std::string& key, int count = 1) = 0;

    // 重置限流器
    virtual void reset(const std::string& key) = 0;

    // 重置所有
    virtual void reset_all() = 0;

    // 获取剩余配额
    virtual int64_t get_remaining(const std::string& key) = 0;
};

// ============================================
// 令牌桶限流器
// ============================================

class TokenBucketRateLimiter : public RateLimiter {
public:
    TokenBucketRateLimiter(int64_t capacity, double refill_rate_per_second)
        : capacity_(capacity), refill_rate_per_second_(refill_rate_per_second) {}

    virtual ~TokenBucketRateLimiter() = default;

    virtual RateLimitResult check(const std::string& key) override;

    virtual bool consume(const std::string& key, int count = 1) override;

    virtual void reset(const std::string& key) override;

    virtual void reset_all() override;

    virtual int64_t get_remaining(const std::string& key) override;

private:
    struct Bucket {
        int64_t tokens;
        std::int64_t last_refill_time_ms;

        Bucket() : tokens(0), last_refill_time_ms(0) {}
    };

    int64_t capacity_;
    double refill_rate_per_second_;
    std::unordered_map<std::string, Bucket> buckets_;
    mutable std::mutex mutex_;

    // 获取当前时间戳（毫秒）
    std::int64_t current_timestamp_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // 补充令牌
    void refill_tokens(Bucket& bucket);
};

// ============================================
// 漏桶限流器
// ============================================

class LeakyBucketRateLimiter : public RateLimiter {
public:
    LeakyBucketRateLimiter(int64_t capacity, double leak_rate_per_second)
        : capacity_(capacity), leak_rate_per_second_(leak_rate_per_second) {}

    virtual ~LeakyBucketRateLimiter() = default;

    virtual RateLimitResult check(const std::string& key) override;

    virtual bool consume(const std::string& key, int count = 1) override;

    virtual void reset(const std::string& key) override;

    virtual void reset_all() override;

    virtual int64_t get_remaining(const std::string& key) override;

private:
    struct Bucket {
        int64_t tokens;
        std::int64_t last_leak_time_ms;

        Bucket() : tokens(0), last_leak_time_ms(0) {}
    };

    int64_t capacity_;
    double leak_rate_per_second_;
    std::unordered_map<std::string, Bucket> buckets_;
    mutable std::mutex mutex_;

    // 获取当前时间戳（毫秒）
    std::int64_t current_timestamp_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // 漏出令牌
    void leak_tokens(Bucket& bucket);
};

// ============================================
// 固定窗口限流器
// ============================================

class FixedWindowRateLimiter : public RateLimiter {
public:
    FixedWindowRateLimiter(int64_t max_requests, int64_t window_size_ms)
        : max_requests_(max_requests), window_size_ms_(window_size_ms) {}

    virtual ~FixedWindowRateLimiter() = default;

    virtual RateLimitResult check(const std::string& key) override;

    virtual bool consume(const std::string& key, int count = 1) override;

    virtual void reset(const std::string& key) override;

    virtual void reset_all() override;

    virtual int64_t get_remaining(const std::string& key) override;

private:
    struct Window {
        int64_t count;
        std::int64_t start_time_ms;

        Window() : count(0), start_time_ms(0) {}
    };

    int64_t max_requests_;
    int64_t window_size_ms_;
    std::unordered_map<std::string, Window> windows_;
    mutable std::mutex mutex_;

    // 获取当前时间戳（毫秒）
    std::int64_t current_timestamp_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // 检查并重置窗口
    bool should_reset_window(Window& window);
};

} // namespace ratelimit
} // namespace chwell
