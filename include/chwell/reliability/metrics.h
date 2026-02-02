#pragma once

#include <atomic>
#include <chrono>
#include <unordered_map>
#include <string>
#include <mutex>

namespace chwell {
namespace reliability {

// 简单的监控指标收集器
class Metrics {
public:
    static Metrics& instance() {
        static Metrics inst;
        return inst;
    }

    // QPS相关
    void increment_qps() { qps_.fetch_add(1, std::memory_order_relaxed); }
    std::int64_t get_qps() const { return qps_.load(std::memory_order_relaxed); }
    void reset_qps() { qps_.store(0, std::memory_order_relaxed); }

    // 在线数
    void increment_online() { online_count_.fetch_add(1, std::memory_order_relaxed); }
    void decrement_online() { online_count_.fetch_sub(1, std::memory_order_relaxed); }
    std::int64_t get_online_count() const { return online_count_.load(std::memory_order_relaxed); }

    // 延迟统计（简化实现）
    void record_latency(const std::string& operation, int milliseconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        latency_sum_[operation] += milliseconds;
        latency_count_[operation]++;
    }

    double get_avg_latency(const std::string& operation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it_sum = latency_sum_.find(operation);
        auto it_count = latency_count_.find(operation);
        if (it_sum != latency_sum_.end() && it_count != latency_count_.end() && it_count->second > 0) {
            return static_cast<double>(it_sum->second) / it_count->second;
        }
        return 0.0;
    }

private:
    Metrics() : qps_(0), online_count_(0) {}

    std::atomic<std::int64_t> qps_;
    std::atomic<std::int64_t> online_count_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::int64_t> latency_sum_;
    std::unordered_map<std::string, std::int64_t> latency_count_;
};

} // namespace reliability
} // namespace chwell
