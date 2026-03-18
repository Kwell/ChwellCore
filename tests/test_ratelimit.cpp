#include <gtest/gtest.h>

#include "chwell/ratelimit/rate_limiter.h"
#include "chwell/core/logger.h"
#include <thread>

using namespace chwell;

namespace {

// ============================================
// 限流器单元测试
// ============================================

TEST(RateLimitTest, TokenBucketBasicConsume) {
    ratelimit::TokenBucketRateLimiter limiter(10, 5.0); // 容量10，每秒5个

    EXPECT_TRUE(limiter.consume("user_1"));
    EXPECT_TRUE(limiter.consume("user_1"));
    EXPECT_TRUE(limiter.consume("user_1"));
    EXPECT_EQ(7, limiter.get_remaining("user_1"));
}

TEST(RateLimitTest, TokenBucketConsumeMultiple) {
    ratelimit::TokenBucketRateLimiter limiter(10, 5.0);

    EXPECT_TRUE(limiter.consume("user_1", 3));
    EXPECT_EQ(7, limiter.get_remaining("user_1"));

    EXPECT_TRUE(limiter.consume("user_1", 5));
    EXPECT_EQ(2, limiter.get_remaining("user_1"));
}

TEST(RateLimitTest, TokenBucketExhausted) {
    ratelimit::TokenBucketRateLimiter limiter(5, 1.0); // 容量5，每秒1个

    // 消耗完所有令牌
    limiter.consume("user_1", 5);

    // 应该失败
    EXPECT_FALSE(limiter.consume("user_1"));
    EXPECT_EQ(0, limiter.get_remaining("user_1"));
}

TEST(RateLimitTest, TokenBucketRefill) {
    ratelimit::TokenBucketRateLimiter limiter(5, 10.0); // 容量5，每秒10个

    // 消耗完所有令牌
    limiter.consume("user_1", 5);

    // 等待100ms，应该补充1个令牌
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 应该有1个令牌
    EXPECT_TRUE(limiter.consume("user_1"));
    EXPECT_EQ(0, limiter.get_remaining("user_1"));
}

TEST(RateLimitTest, TokenBucketReset) {
    ratelimit::TokenBucketRateLimiter limiter(10, 5.0);

    limiter.consume("user_1", 5);
    EXPECT_EQ(5, limiter.get_remaining("user_1"));

    limiter.reset("user_1");
    EXPECT_EQ(10, limiter.get_remaining("user_1"));
}

TEST(RateLimitTest, TokenBucketMultipleKeys) {
    ratelimit::TokenBucketRateLimiter limiter(10, 5.0);

    limiter.consume("user_1", 5);
    limiter.consume("user_2", 3);

    EXPECT_EQ(5, limiter.get_remaining("user_1"));
    EXPECT_EQ(7, limiter.get_remaining("user_2"));
}

TEST(RateLimitTest, LeakyBucketBasicConsume) {
    ratelimit::LeakyBucketRateLimiter limiter(10, 5.0);

    EXPECT_TRUE(limiter.consume("user_1"));
    EXPECT_TRUE(limiter.consume("user_1"));
    EXPECT_TRUE(limiter.consume("user_1"));
    EXPECT_EQ(7, limiter.get_remaining("user_1"));
}

TEST(RateLimitTest, LeakyBucketConsumeMultiple) {
    ratelimit::LeakyBucketRateLimiter limiter(10, 5.0);

    EXPECT_TRUE(limiter.consume("user_1", 3));
    EXPECT_EQ(7, limiter.get_remaining("user_1"));

    EXPECT_TRUE(limiter.consume("user_1", 5));
    EXPECT_EQ(2, limiter.get_remaining("user_1"));
}

TEST(RateLimitTest, LeakyBucketExhausted) {
    ratelimit::LeakyBucketRateLimiter limiter(5, 1.0);

    // 消耗完所有令牌
    limiter.consume("user_1", 5);

    // 应该失败
    EXPECT_FALSE(limiter.consume("user_1"));
    EXPECT_EQ(0, limiter.get_remaining("user_1"));
}

TEST(RateLimitTest, LeakyBucketRefill) {
    ratelimit::LeakyBucketRateLimiter limiter(5, 10.0);

    // 消耗完所有令牌
    limiter.consume("user_1", 5);

    // 等待100ms，应该补充1个令牌
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 应该有1个令牌
    EXPECT_TRUE(limiter.consume("user_1"));
    EXPECT_EQ(0, limiter.get_remaining("user_1"));
}

TEST(RateLimitTest, LeakyBucketReset) {
    ratelimit::LeakyBucketRateLimiter limiter(10, 5.0);

    limiter.consume("user_1", 5);
    EXPECT_EQ(5, limiter.get_remaining("user_1"));

    limiter.reset("user_1");
    EXPECT_EQ(10, limiter.get_remaining("user_1"));
}

TEST(RateLimitTest, FixedWindowBasicConsume) {
    ratelimit::FixedWindowRateLimiter limiter(10, 1000); // 10请求，1秒窗口

    auto result1 = limiter.check("user_1");
    EXPECT_TRUE(result1.allowed);
    EXPECT_EQ(9, result1.remaining);

    auto result2 = limiter.check("user_1");
    EXPECT_TRUE(result2.allowed);
    EXPECT_EQ(8, result2.remaining);
}

TEST(RateLimitTest, FixedWindowExhausted) {
    ratelimit::FixedWindowRateLimiter limiter(3, 1000); // 3请求，1秒窗口

    // 消耗完所有配额
    limiter.check("user_1");
    limiter.check("user_1");
    limiter.check("user_1");

    // 应该被阻止
    auto result = limiter.check("user_1");
    EXPECT_FALSE(result.allowed);
    EXPECT_EQ(0, result.remaining);
    EXPECT_FALSE(result.reason.empty());
}

TEST(RateLimitTest, FixedWindowReset) {
    ratelimit::FixedWindowRateLimiter limiter(10, 100); // 10请求，100ms窗口

    // 消耗部分配额
    limiter.check("user_1");
    limiter.check("user_1");
    limiter.check("user_1");

    EXPECT_EQ(7, limiter.get_remaining("user_1"));

    // 手动重置
    limiter.reset("user_1");

    EXPECT_EQ(10, limiter.get_remaining("user_1"));
}

TEST(RateLimitTest, FixedWindowAutoReset) {
    ratelimit::FixedWindowRateLimiter limiter(10, 100); // 10请求，100ms窗口

    // 消耗完所有配额
    for (int i = 0; i < 10; i++) {
        limiter.check("user_1");
    }

    auto result = limiter.check("user_1");
    EXPECT_FALSE(result.allowed);

    // 等待窗口过期
    std::this_thread::sleep_for(std::chrono::milliseconds(110));

    // 应该自动重置
    result = limiter.check("user_1");
    EXPECT_TRUE(result.allowed);
    EXPECT_EQ(9, result.remaining);
}

TEST(RateLimitTest, FixedWindowMultipleKeys) {
    ratelimit::FixedWindowRateLimiter limiter(10, 1000);

    limiter.check("user_1");
    limiter.check("user_1");
    limiter.check("user_2");
    limiter.check("user_2");

    EXPECT_EQ(8, limiter.get_remaining("user_1"));
    EXPECT_EQ(8, limiter.get_remaining("user_2"));
}

TEST(RateLimitTest, FixedWindowWaitTime) {
    ratelimit::FixedWindowRateLimiter limiter(5, 1000);

    // 消耗完所有配额
    for (int i = 0; i < 5; i++) {
        limiter.check("user_1");
    }

    auto result = limiter.check("user_1");
    EXPECT_FALSE(result.allowed);
    EXPECT_GT(result.wait_time_ms, 0);
}

} // namespace
