#include "chwell/circuitbreaker/circuit_breaker.h"
#include <algorithm>

namespace chwell {
namespace circuitbreaker {

// ============================================
// DefaultCircuitBreaker
// ============================================

CircuitBreakerResult DefaultCircuitBreaker::execute(std::function<void()> func) {
    CircuitBreakerResult result;
    result.success = false;
    result.state = get_state();

    // 检查熔断器状态
    if (result.state == CircuitState::OPEN) {
        result.reason = "Circuit breaker is OPEN";
        return result;
    }

    if (result.state == CircuitState::HALF_OPEN) {
        uint32_t count = half_open_count_.fetch_add(1, std::memory_order_relaxed);
        if (count >= config_.half_open_calls) {
            // 半开状态超过调用次数，回到打开状态
            result.reason = "Half-open calls exceeded";
            return result;
        }
    }

    auto start_time = std::chrono::system_clock::now();

    try {
        // 执行函数
        func();

        auto end_time = std::chrono::system_clock::now();
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();

        result.success = true;
        result.state = get_state();

        // 成功调用
        success_count_++;

        // 如果在半开状态，成功后回到关闭状态
        if (result.state == CircuitState::HALF_OPEN) {
            state_.store(CircuitState::CLOSED, std::memory_order_relaxed);
            CHWELL_LOG_INFO("Circuit breaker " + name_ + " recovered to CLOSED state");
        }

        return result;
    } catch (const std::exception& e) {
        auto end_time = std::chrono::system_clock::now();
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();

        result.reason = std::string("Exception: ") + e.what();
        result.success = false;
        result.state = get_state();

        // 失败调用
        failure_count_++;
        last_failure_time_ms_.store(current_timestamp_ms(), std::memory_order_relaxed);

        // 检查是否需要熔断
        if (should_trip()) {
            state_.store(CircuitState::OPEN, std::memory_order_relaxed);
            CHWELL_LOG_WARN("Circuit breaker " + name_ + " tripped to OPEN state");
        }

        return result;
    }
}

void DefaultCircuitBreaker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);

    state_.store(CircuitState::CLOSED, std::memory_order_relaxed);
    failure_count_.store(0, std::memory_order_relaxed);
    success_count_.store(0, std::memory_order_relaxed);
    half_open_count_.store(0, std::memory_order_relaxed);
    last_failure_time_ms_.store(0, std::memory_order_relaxed);

    CHWELL_LOG_INFO("Circuit breaker " + name_ + " reset");
}

double DefaultCircuitBreaker::get_failure_rate() const {
    uint64_t total = get_failure_count() + get_success_count();
    if (total == 0) {
        return 0.0;
    }

    return static_cast<double>(get_failure_count()) / static_cast<double>(total);
}

void DefaultCircuitBreaker::record_success() {
    success_count_++;
    if (state_.load(std::memory_order_relaxed) == CircuitState::HALF_OPEN) {
        state_.store(CircuitState::CLOSED, std::memory_order_relaxed);
        CHWELL_LOG_INFO("Circuit breaker " + name_ + " recovered to CLOSED state");
    }
}

void DefaultCircuitBreaker::record_failure(const std::string& reason) {
    failure_count_++;
    last_failure_time_ms_.store(current_timestamp_ms(), std::memory_order_relaxed);
    if (should_trip()) {
        state_.store(CircuitState::OPEN, std::memory_order_relaxed);
        CHWELL_LOG_WARN("Circuit breaker " + name_ + " tripped to OPEN state: " + reason);
    }
}

void DefaultCircuitBreaker::trip() {
    state_.store(CircuitState::OPEN, std::memory_order_relaxed);
    last_failure_time_ms_.store(current_timestamp_ms(), std::memory_order_relaxed);

    CHWELL_LOG_WARN("Circuit breaker " + name_ + " manually tripped to OPEN state");
}

void DefaultCircuitBreaker::recover() {
    state_.store(CircuitState::CLOSED, std::memory_order_relaxed);

    CHWELL_LOG_INFO("Circuit breaker " + name_ + " manually recovered to CLOSED state");
}

bool DefaultCircuitBreaker::should_trip() const {
    uint64_t failures = get_failure_count();
    uint64_t total = failures + get_success_count();

    switch (config_.trip_strategy) {
        case TripStrategy::FAILURE_COUNT:
            return failures >= config_.failure_threshold;

        case TripStrategy::FAILURE_RATE:
            // 至少需要 failure_threshold 个样本才能判断失败率
            if (total < static_cast<uint64_t>(config_.failure_threshold)) {
                return false;
            }
            return get_failure_rate() >= config_.failure_rate_threshold;

        case TripStrategy::FAILURE_COUNT_OR_RATE:
            return (failures >= config_.failure_threshold) ||
                   (total > 0 && get_failure_rate() >= config_.failure_rate_threshold);

        default:
            return false;
    }
}

bool DefaultCircuitBreaker::should_attempt_reset() const {
    if (state_.load(std::memory_order_relaxed) != CircuitState::OPEN) {
        return false;
    }

    uint64_t last_failure = last_failure_time_ms_.load(std::memory_order_relaxed);
    uint64_t now = current_timestamp_ms();

    // 检查是否超时
    if (now - last_failure >= config_.timeout_ms) {
        return true;
    }

    return false;
}

} // namespace circuitbreaker
} // namespace chwell
