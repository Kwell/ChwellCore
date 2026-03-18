#pragma once

#include "chwell/core/logger.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace chwell {
namespace circuitbreaker {

// 熔断器状态
enum class CircuitState {
    CLOSED,    // 关闭（正常）
    OPEN,      // 打开（熔断）
    HALF_OPEN  // 半开（试探）
};

// 熔断器策略
enum class TripStrategy {
    FAILURE_COUNT,      // 失败次数
    FAILURE_RATE,       // 失败率
    FAILURE_COUNT_OR_RATE, // 失败次数或率
};

// 熔断器结果
struct CircuitBreakerResult {
    bool success;           // 是否成功
    CircuitState state;    // 熔断器状态
    std::string reason;     // 失败原因
    uint64_t duration_ms;  // 耗时（毫秒）

    CircuitBreakerResult()
        : success(false), state(CircuitState::CLOSED), duration_ms(0) {}
};

// 熔断器接口
class CircuitBreaker {
public:
    virtual ~CircuitBreaker() = default;

    // 执行函数（带熔断保护）
    virtual CircuitBreakerResult execute(std::function<void()> func) = 0;

    // 执行函数（带返回值）
    template<typename T>
    CircuitBreakerResult execute_with_result(std::function<T()> func, T& out_result);

    // 获取当前状态
    virtual CircuitState get_state() const = 0;

    // 重置熔断器
    virtual void reset() = 0;

    // 获取失败次数
    virtual uint64_t get_failure_count() const = 0;

    // 获取成功次数
    virtual uint64_t get_success_count() const = 0;

    // 获取失败率
    virtual double get_failure_rate() const = 0;
};

// ============================================
// 熔断器配置
// ============================================

struct CircuitBreakerConfig {
    TripStrategy trip_strategy = TripStrategy::FAILURE_COUNT_OR_RATE;
    uint32_t failure_threshold = 5;        // 失败阈值
    double failure_rate_threshold = 0.5;     // 失败率阈值
    uint64_t timeout_ms = 60000;            // 熔断超时（毫秒）
    uint64_t half_open_calls = 3;           // 半开状态允许调用次数

    CircuitBreakerConfig() = default;
};

// ============================================
// 熔断器实现
// ============================================

class DefaultCircuitBreaker : public CircuitBreaker {
public:
    DefaultCircuitBreaker(const std::string& name, const CircuitBreakerConfig& config)
        : name_(name), config_(config), state_(CircuitState::CLOSED),
          failure_count_(0), success_count_(0), half_open_count_(0) {}

    virtual ~DefaultCircuitBreaker() = default;

    // 执行函数（带熔断保护）
    virtual CircuitBreakerResult execute(std::function<void()> func) override;

    // 获取当前状态
    virtual CircuitState get_state() const override {
        return state_.load();
    }

    // 重置熔断器
    virtual void reset() override;

    // 获取失败次数
    virtual uint64_t get_failure_count() const override {
        return failure_count_.load();
    }

    // 获取成功次数
    virtual uint64_t get_success_count() const override {
        return success_count_.load();
    }

    // 获取失败率
    virtual double get_failure_rate() const override;

    // 手动触发熔断
    void trip();

    // 手动恢复
    void recover();

private:
    // 获取当前时间戳（毫秒）
    std::uint64_t current_timestamp_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // 检查是否需要熔断
    bool should_trip() const;

    // 检查是否需要恢复到半开状态
    bool should_attempt_reset() const;

    std::string name_;
    CircuitBreakerConfig config_;
    std::atomic<CircuitState> state_;
    std::atomic<uint64_t> failure_count_;
    std::atomic<uint64_t> success_count_;
    std::atomic<uint32_t> half_open_count_;
    std::atomic<uint64_t> last_failure_time_ms_;
    mutable std::mutex mutex_;
};

// ============================================
// 模板方法实现
// ============================================

template<typename T>
CircuitBreakerResult CircuitBreaker::execute_with_result(std::function<T()> func, T& out_result) {
    CircuitBreakerResult result;
    result.success = false;
    result.state = get_state();

    // 检查熔断器状态
    if (result.state == CircuitState::OPEN) {
        result.reason = "Circuit breaker is OPEN";
        return result;
    }

    auto start_time = std::chrono::system_clock::now();

    try {
        // 执行函数
        T result_value = func();
        out_result = result_value;

        auto end_time = std::chrono::system_clock::now();
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();

        result.success = true;
        result.state = get_state();

        // 根据熔断器类型调用相应方法
        DefaultCircuitBreaker* cb = dynamic_cast<DefaultCircuitBreaker*>(this);
        if (cb) {
            // 成功调用
        }

        return result;
    } catch (const std::exception& e) {
        auto end_time = std::chrono::system_clock::now();
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();

        result.reason = std::string("Exception: ") + e.what();
        result.success = false;
        result.state = get_state();

        // 根据熔断器类型调用相应方法
        DefaultCircuitBreaker* cb = dynamic_cast<DefaultCircuitBreaker*>(this);
        if (cb) {
            // 失败调用
        }

        return result;
    }
}

} // namespace circuitbreaker
} // namespace chwell
