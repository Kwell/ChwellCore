#include <gtest/gtest.h>

#include "chwell/circuitbreaker/circuit_breaker.h"
#include "chwell/core/logger.h"
#include <thread>

using namespace chwell;

namespace {

// ============================================
// 熔断器单元测试
// ============================================

TEST(CircuitBreakerTest, BasicExecuteSuccess) {
    circuitbreaker::CircuitBreakerConfig config;
    config.failure_threshold = 3;
    config.failure_rate_threshold = 0.5;

    circuitbreaker::DefaultCircuitBreaker cb("test_breaker", config);

    int call_count = 0;
    auto func = [&call_count]() {
        call_count++;
    };

    auto result = cb.execute(func);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(circuitbreaker::CircuitState::CLOSED, result.state);
    EXPECT_EQ(1, call_count);
    EXPECT_EQ(1, cb.get_success_count());
    EXPECT_EQ(0, cb.get_failure_count());
}

TEST(CircuitBreakerTest, BasicExecuteFailure) {
    circuitbreaker::CircuitBreakerConfig config;
    config.failure_threshold = 3;
    config.failure_rate_threshold = 0.5;

    circuitbreaker::DefaultCircuitBreaker cb("test_breaker", config);

    auto func = []() {
        throw std::runtime_error("Simulated failure");
    };

    auto result = cb.execute(func);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(circuitbreaker::CircuitState::CLOSED, result.state);
    EXPECT_FALSE(result.reason.empty());
    EXPECT_EQ(1, cb.get_failure_count());
    EXPECT_EQ(0, cb.get_success_count());
}

TEST(CircuitBreakerTest, TripOnFailureCount) {
    circuitbreaker::CircuitBreakerConfig config;
    config.trip_strategy = circuitbreaker::TripStrategy::FAILURE_COUNT;
    config.failure_threshold = 2;
    config.failure_rate_threshold = 0.5;

    circuitbreaker::DefaultCircuitBreaker cb("test_breaker", config);

    auto fail_func = []() {
        throw std::runtime_error("Simulated failure");
    };

    // 第一次失败，不应触发熔断（失败次数 1 < threshold 2）
    cb.execute(fail_func);
    EXPECT_EQ(circuitbreaker::CircuitState::CLOSED, cb.get_state());

    // 第二次失败，应该熔断（失败次数 2 >= threshold 2）
    cb.execute(fail_func);
    EXPECT_EQ(circuitbreaker::CircuitState::OPEN, cb.get_state());
    EXPECT_EQ(2u, cb.get_failure_count());
}

TEST(CircuitBreakerTest, TripOnFailureRate) {
    circuitbreaker::CircuitBreakerConfig config;
    config.trip_strategy = circuitbreaker::TripStrategy::FAILURE_RATE;
    // failure_threshold 作为最小样本数：至少 3 次调用才开始按失败率判断
    config.failure_threshold = 3;
    config.failure_rate_threshold = 0.5;

    circuitbreaker::DefaultCircuitBreaker cb("test_breaker", config);

    auto fail_func = []() {
        throw std::runtime_error("Simulated failure");
    };

    // 前 2 次失败：样本数 2 < threshold=3，即使失败率 100%，不应熔断
    cb.execute(fail_func);
    EXPECT_EQ(circuitbreaker::CircuitState::CLOSED, cb.get_state());
    cb.execute(fail_func);
    EXPECT_EQ(circuitbreaker::CircuitState::CLOSED, cb.get_state());

    // 第 3 次失败：total=3 >= threshold=3，失败率 100% >= 50% → 熔断
    cb.execute(fail_func);
    EXPECT_EQ(circuitbreaker::CircuitState::OPEN, cb.get_state());
    EXPECT_DOUBLE_EQ(1.0, cb.get_failure_rate());
    EXPECT_EQ(3u, cb.get_failure_count());
}

TEST(CircuitBreakerTest, OpenStateBlocksRequests) {
    circuitbreaker::CircuitBreakerConfig config;
    config.failure_threshold = 2;
    config.failure_rate_threshold = 0.5;

    circuitbreaker::DefaultCircuitBreaker cb("test_breaker", config);

    auto fail_func = []() {
        throw std::runtime_error("Simulated failure");
    };

    // 触发熔断
    cb.execute(fail_func);
    cb.execute(fail_func);

    EXPECT_EQ(circuitbreaker::CircuitState::OPEN, cb.get_state());

    // 尝试调用，应该被阻止
    auto result = cb.execute([]() {});

    EXPECT_FALSE(result.success);
    EXPECT_EQ(circuitbreaker::CircuitState::OPEN, result.state);
    EXPECT_FALSE(result.reason.empty());
}

TEST(CircuitBreakerTest, ResetFromOpen) {
    circuitbreaker::CircuitBreakerConfig config;
    config.trip_strategy = circuitbreaker::TripStrategy::FAILURE_COUNT;
    config.failure_threshold = 2;
    config.failure_rate_threshold = 0.5;
    config.timeout_ms = 300;

    circuitbreaker::DefaultCircuitBreaker cb("test_breaker", config);

    auto fail_func = []() {
        throw std::runtime_error("Simulated failure");
    };

    // 触发熔断
    cb.execute(fail_func);
    cb.execute(fail_func);
    EXPECT_EQ(circuitbreaker::CircuitState::OPEN, cb.get_state());

    // 等待超时后手动恢复（simulate HALF_OPEN -> CLOSED via successful call）
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // 手动恢复到 CLOSED，再次执行应该成功
    cb.recover();
    auto result = cb.execute([]() {});
    EXPECT_TRUE(result.success);
    EXPECT_EQ(circuitbreaker::CircuitState::CLOSED, cb.get_state());
}

TEST(CircuitBreakerTest, ExecuteWithResult) {
    circuitbreaker::CircuitBreakerConfig config;
    config.failure_threshold = 3;
    config.failure_rate_threshold = 0.5;

    circuitbreaker::DefaultCircuitBreaker cb("test_breaker", config);

    int result_value = 42;
    auto func = [&result_value]() -> int {
        return result_value;
    };

    int out_result = 0;
    auto cb_result = cb.execute_with_result<int>(func, out_result);

    EXPECT_TRUE(cb_result.success);
    EXPECT_EQ(circuitbreaker::CircuitState::CLOSED, cb_result.state);
    EXPECT_EQ(42, out_result);
}

TEST(CircuitBreakerTest, ManualReset) {
    circuitbreaker::CircuitBreakerConfig config;
    config.failure_threshold = 2;
    config.failure_rate_threshold = 0.5;

    circuitbreaker::DefaultCircuitBreaker cb("test_breaker", config);

    auto fail_func = []() {
        throw std::runtime_error("Simulated failure");
    };

    // 触发熔断
    cb.execute(fail_func);
    cb.execute(fail_func);

    EXPECT_EQ(circuitbreaker::CircuitState::OPEN, cb.get_state());

    // 手动重置
    cb.reset();

    EXPECT_EQ(circuitbreaker::CircuitState::CLOSED, cb.get_state());
    EXPECT_EQ(0, cb.get_failure_count());
    EXPECT_EQ(0, cb.get_success_count());
}

TEST(CircuitBreakerTest, ManualTrip) {
    circuitbreaker::CircuitBreakerConfig config;
    config.failure_threshold = 5;
    config.failure_rate_threshold = 0.5;

    circuitbreaker::DefaultCircuitBreaker cb("test_breaker", config);

    // 手动触发熔断
    cb.trip();

    EXPECT_EQ(circuitbreaker::CircuitState::OPEN, cb.get_state());

    // 尝试调用，应该被阻止
    auto result = cb.execute([]() {});

    EXPECT_FALSE(result.success);
    EXPECT_EQ(circuitbreaker::CircuitState::OPEN, result.state);
}

TEST(CircuitBreakerTest, ManualRecover) {
    circuitbreaker::CircuitBreakerConfig config;
    config.failure_threshold = 5;
    config.failure_rate_threshold = 0.5;

    circuitbreaker::DefaultCircuitBreaker cb("test_breaker", config);

    auto fail_func = []() {
        throw std::runtime_error("Simulated failure");
    };

    // 触发熔断
    cb.execute(fail_func);
    cb.execute(fail_func);
    cb.execute(fail_func);
    cb.execute(fail_func);
    cb.execute(fail_func);

    EXPECT_EQ(circuitbreaker::CircuitState::OPEN, cb.get_state());

    // 手动恢复
    cb.recover();

    EXPECT_EQ(circuitbreaker::CircuitState::CLOSED, cb.get_state());

    // 可以正常调用
    auto result = cb.execute([]() {});

    EXPECT_TRUE(result.success);
    EXPECT_EQ(circuitbreaker::CircuitState::CLOSED, result.state);
}

TEST(CircuitBreakerTest, FailureRateCalculation) {
    circuitbreaker::CircuitBreakerConfig config;
    config.trip_strategy = circuitbreaker::TripStrategy::FAILURE_COUNT;
    config.failure_threshold = 10;
    config.failure_rate_threshold = 0.5;

    circuitbreaker::DefaultCircuitBreaker cb("test_breaker", config);

    auto fail_func = []() {
        throw std::runtime_error("Simulated failure");
    };

    auto success_func = []() {};

    // 3次失败，1次成功，失败率 75%
    cb.execute(fail_func);
    cb.execute(fail_func);
    cb.execute(fail_func);
    cb.execute(success_func);

    EXPECT_DOUBLE_EQ(0.75, cb.get_failure_rate());
    EXPECT_EQ(3u, cb.get_failure_count());
    EXPECT_EQ(1u, cb.get_success_count());
}

} // namespace
