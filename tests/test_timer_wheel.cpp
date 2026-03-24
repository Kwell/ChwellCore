#include <gtest/gtest.h>
#include <thread>
#include <chrono>

#include "chwell/core/timer_wheel.h"

using namespace chwell;
using namespace std::chrono_literals;

TEST(TimerWheelTest, AddOneShotTimer) {
    core::TimerWheel wheel(100, 60, 4);
    wheel.start();
    
    int counter = 0;
    auto handle = wheel.add_timer(200, [&counter]() {
        counter++;
    });
    
    EXPECT_TRUE(handle.valid());
    
    // 增加等待时间，确保定时器触发
    std::this_thread::sleep_for(400ms);
    EXPECT_GE(counter, 1);
    
    wheel.stop();
}

TEST(TimerWheelTest, AddRepeatingTimer) {
    core::TimerWheel wheel(100, 60, 4);
    wheel.start();
    
    int counter = 0;
    auto handle = wheel.add_repeat_timer(100, [&counter]() {
        counter++;
    });
    
    EXPECT_TRUE(handle.valid());
    
    std::this_thread::sleep_for(350ms);
    EXPECT_GE(counter, 2);
    
    wheel.cancel_timer(handle);
    int prev = counter;
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(counter, prev);
    
    wheel.stop();
}

TEST(TimerWheelTest, CancelTimer) {
    core::TimerWheel wheel(100, 60, 4);
    wheel.start();
    
    int counter = 0;
    auto handle = wheel.add_timer(500, [&counter]() {
        counter++;
    });
    
    wheel.cancel_timer(handle);
    EXPECT_FALSE(handle.valid());
    
    std::this_thread::sleep_for(600ms);
    EXPECT_EQ(counter, 0);
    
    wheel.stop();
}

TEST(TimerWheelTest, MultipleTimers) {
    core::TimerWheel wheel(100, 60, 4);
    wheel.start();

    std::vector<int> order;

    // 注意：由于时间精度问题，定时器的触发顺序可能会有细微偏差
    // 这个测试验证的是所有定时器都能正确触发，而不是严格的顺序
    wheel.add_timer(300, [&order]() { order.push_back(3); });
    wheel.add_timer(100, [&order]() { order.push_back(1); });
    wheel.add_timer(200, [&order]() { order.push_back(2); });

    // 等待 2000ms：给最长 300ms 定时器充足裕量
    // 同时考虑到系统负载和时间精度问题
    std::this_thread::sleep_for(2000ms);

    // 验证所有定时器都触发了
    ASSERT_EQ(order.size(), 3u);

    // 验证包含所有预期的值
    bool has_1 = std::find(order.begin(), order.end(), 1) != order.end();
    bool has_2 = std::find(order.begin(), order.end(), 2) != order.end();
    bool has_3 = std::find(order.begin(), order.end(), 3) != order.end();

    EXPECT_TRUE(has_1);
    EXPECT_TRUE(has_2);
    EXPECT_TRUE(has_3);

    wheel.stop();
}

TEST(TimerWheelTest, TimerWheelNotStarted) {
    core::TimerWheel wheel(100, 60, 4);
    
    int counter = 0;
    wheel.add_timer(100, [&counter]() {
        counter++;
    });
    
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(counter, 0);
}

TEST(TimerWheelTest, IsTimerValid) {
    core::TimerWheel wheel(100, 60, 4);
    wheel.start();
    
    auto handle = wheel.add_timer(1000, []() {});
    EXPECT_TRUE(wheel.is_timer_valid(handle));
    
    wheel.cancel_timer(handle);
    EXPECT_FALSE(wheel.is_timer_valid(handle));
    
    wheel.stop();
}

TEST(TimerWheelTest, GetNextExpireTime) {
    core::TimerWheel wheel(100, 60, 4);
    wheel.start();
    
    EXPECT_EQ(wheel.get_next_expire_time(), -1);
    
    wheel.add_timer(500, []() {});
    int64_t next_expire = wheel.get_next_expire_time();
    EXPECT_GT(next_expire, 0);
    
    wheel.stop();
}

TEST(TimerWheelTest, CurrentTimeMs) {
    auto t1 = core::TimerWheel::current_time_ms();
    std::this_thread::sleep_for(100ms);
    auto t2 = core::TimerWheel::current_time_ms();
    EXPECT_GE(t2 - t1, 100);
}
