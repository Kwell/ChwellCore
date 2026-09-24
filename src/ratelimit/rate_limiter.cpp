#include "chwell/ratelimit/rate_limiter.h"
#include <algorithm>

namespace chwell {
namespace ratelimit {

// ============================================
// TokenBucketRateLimiter
// ============================================

RateLimitResult TokenBucketRateLimiter::check(const std::string& key) {
    RateLimitResult result;
    result.allowed = consume(key, 1);
    result.remaining = get_remaining(key);
    return result;
}

bool TokenBucketRateLimiter::consume(const std::string& key, int count) {
    if (count <= 0) return true;
    std::lock_guard<std::mutex> lock(mutex_);

    int64_t now = current_timestamp_ms();
    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
        // 创建新桶
        Bucket bucket;
        bucket.tokens = capacity_;
        bucket.last_refill_time_ms = now;
        bucket.last_access_time_ms = now;
        buckets_[key] = bucket;
        it = buckets_.find(key);
    }

    Bucket& bucket = it->second;
    bucket.last_access_time_ms = now;  // 更新访问时间

    // 补充令牌
    refill_tokens(bucket);

    // 检查是否有足够令牌
    if (bucket.tokens >= count) {
        bucket.tokens -= count;
        return true;
    }

    return false;
}

void TokenBucketRateLimiter::reset(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = buckets_.find(key);
    if (it != buckets_.end()) {
        it->second.tokens = capacity_;
        it->second.last_refill_time_ms = current_timestamp_ms();
    }
}

void TokenBucketRateLimiter::reset_all() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : buckets_) {
        pair.second.tokens = capacity_;
        pair.second.last_refill_time_ms = current_timestamp_ms();
    }
}

int64_t TokenBucketRateLimiter::get_remaining(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
        return capacity_;
    }

    Bucket& bucket = it->second;
    refill_tokens(bucket);

    return bucket.tokens;
}

void TokenBucketRateLimiter::refill_tokens(Bucket& bucket) {
    int64_t now = current_timestamp_ms();
    int64_t elapsed_ms = now - bucket.last_refill_time_ms;

    if (elapsed_ms > 0) {
        // 计算应该补充的令牌数
        double tokens_to_add = (elapsed_ms / 1000.0) * refill_rate_per_second_;
        double whole = tokens_to_add; int64_t add = static_cast<int64_t>(whole); bucket.tokens = std::min(capacity_, bucket.tokens + add);
        bucket.last_refill_time_ms = now;
    }
}

void TokenBucketRateLimiter::cleanup_idle(int64_t max_idle_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = current_timestamp_ms();

    for (auto it = buckets_.begin(); it != buckets_.end(); ) {
        if (now - it->second.last_access_time_ms > max_idle_ms) {
            it = buckets_.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================
// LeakyBucketRateLimiter
// ============================================

RateLimitResult LeakyBucketRateLimiter::check(const std::string& key) {
    RateLimitResult result;
    result.allowed = consume(key, 1);
    result.remaining = get_remaining(key);
    return result;
}

bool LeakyBucketRateLimiter::consume(const std::string& key, int count) {
    if (count <= 0) return true;
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
        // 创建新桶
        Bucket bucket;
        bucket.tokens = 0;
        bucket.last_leak_time_ms = current_timestamp_ms();
        buckets_[key] = bucket;
        it = buckets_.find(key);
    }

    Bucket& bucket = it->second;
    leak_tokens(bucket);

    if (bucket.tokens + count <= capacity_) {
        bucket.tokens += count;
        return true;
    }

    return false;
}

void LeakyBucketRateLimiter::reset(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = buckets_.find(key);
    if (it != buckets_.end()) {
        it->second.tokens = capacity_;
        it->second.last_leak_time_ms = current_timestamp_ms();
    }
}

void LeakyBucketRateLimiter::reset_all() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : buckets_) {
        pair.second.tokens = capacity_;
        pair.second.last_leak_time_ms = current_timestamp_ms();
    }
}

int64_t LeakyBucketRateLimiter::get_remaining(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
        return capacity_;
    }

    Bucket& bucket = it->second;
    leak_tokens(bucket);

    return bucket.tokens;
}

void LeakyBucketRateLimiter::leak_tokens(Bucket& bucket) {
    int64_t now = current_timestamp_ms();
    int64_t elapsed_ms = now - bucket.last_leak_time_ms;

    if (elapsed_ms > 0) {
        // 漏桶：水位随时间下降（原实现误为补水）
        double tokens_to_leak = (elapsed_ms / 1000.0) * leak_rate_per_second_;
        bucket.tokens = std::max<int64_t>(
            0, bucket.tokens - static_cast<int64_t>(tokens_to_leak));
        bucket.last_leak_time_ms = now;
    }
}

void LeakyBucketRateLimiter::cleanup_idle(int64_t max_idle_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = current_timestamp_ms();
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        if (now - it->second.last_leak_time_ms > max_idle_ms) {
            it = buckets_.erase(it);
        } else {
            ++it;
        }
    }
}

void FixedWindowRateLimiter::cleanup_idle(int64_t max_idle_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = current_timestamp_ms();
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (now - it->second.start_time_ms > max_idle_ms) {
            it = windows_.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================
// FixedWindowRateLimiter
// ============================================

RateLimitResult FixedWindowRateLimiter::check(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = windows_.find(key);
    if (it == windows_.end()) {
        // 创建新窗口
        Window window;
        window.count = 0;
        window.start_time_ms = current_timestamp_ms();
        windows_[key] = window;
        it = windows_.find(key);
    }

    Window& window = it->second;

    // 检查窗口是否需要重置
    if (should_reset_window(window)) {
        window.count = 0;
        window.start_time_ms = current_timestamp_ms();
    }

    // 检查是否超限
    if (window.count < max_requests_) {
        window.count++;
        RateLimitResult result;
        result.allowed = true;
        result.remaining = max_requests_ - window.count;
        return result;
    }

    // 超限
    RateLimitResult result;
    result.allowed = false;
    result.remaining = 0;
    result.reason = "Fixed window limit exceeded";
    result.wait_time_ms = window_size_ms_ - (current_timestamp_ms() - window.start_time_ms);

    return result;
}

bool FixedWindowRateLimiter::consume(const std::string& key, int count) {
    if (count <= 0) return true;
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = windows_.find(key);
    if (it == windows_.end()) {
        Window window;
        window.count = 0;
        window.start_time_ms = current_timestamp_ms();
        windows_[key] = window;
        it = windows_.find(key);
    }

    Window& window = it->second;
    if (should_reset_window(window)) {
        window.count = 0;
        window.start_time_ms = current_timestamp_ms();
    }

    if (window.count + count <= max_requests_) {
        window.count += count;
        return true;
    }
    return false;
}

void FixedWindowRateLimiter::reset(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = windows_.find(key);
    if (it != windows_.end()) {
        it->second.count = 0;
        it->second.start_time_ms = current_timestamp_ms();
    }
}

void FixedWindowRateLimiter::reset_all() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : windows_) {
        pair.second.count = 0;
        pair.second.start_time_ms = current_timestamp_ms();
    }
}

int64_t FixedWindowRateLimiter::get_remaining(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = windows_.find(key);
    if (it == windows_.end()) {
        return max_requests_;
    }

    Window& window = it->second;

    // 检查窗口是否需要重置
    if (should_reset_window(window)) {
        window.count = 0;
        window.start_time_ms = current_timestamp_ms();
    }

    return max_requests_ - window.count;
}

bool FixedWindowRateLimiter::should_reset_window(Window& window) {
    int64_t now = current_timestamp_ms();
    return (now - window.start_time_ms) >= window_size_ms_;
}

} // namespace ratelimit
} // namespace chwell
