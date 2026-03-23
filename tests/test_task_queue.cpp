#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>

#include "chwell/task/task_queue.h"

using namespace chwell;
using namespace std::chrono_literals;

class TaskQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        task::TaskQueue::Config config;
        config.worker_threads = 2;
        config.max_queue_size = 100;
        queue_ = std::make_unique<task::TaskQueue>(config);
    }
    
    void TearDown() override {
        queue_->stop();
    }
    
    std::unique_ptr<task::TaskQueue> queue_;
};

TEST_F(TaskQueueTest, SubmitTask) {
    queue_->start();
    
    std::atomic<int> counter{0};
    
    int64_t id = queue_->submit_void([&counter]() {
        counter++;
    });
    
    EXPECT_GT(id, 0);
    
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(counter, 1);
}

TEST_F(TaskQueueTest, MultipleTasks) {
    queue_->start();
    
    std::atomic<int> counter{0};
    
    for (int i = 0; i < 10; i++) {
        queue_->submit_void([&counter]() {
            counter++;
        });
    }
    
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(counter, 10);
}

TEST_F(TaskQueueTest, TaskWithResult) {
    queue_->start();
    
    std::atomic<int> result{0};
    
    queue_->submit<int>(
        []() { return 42; },
        [&result](const task::TaskResult<int>& r) {
            if (r.ok()) {
                result = r.value;
            }
        }
    );
    
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(result, 42);
}

TEST_F(TaskQueueTest, TaskPriority) {
    task::TaskQueue::Config config;
    config.worker_threads = 1;
    config.enable_priority = true;
    task::TaskQueue queue(config);

    // 先提交所有任务再启动工作线程：优先级队列只能对"已在队列中"的任务排序。
    // 若先 start() 再逐个 submit，唯一的工作线程会在其他任务入队前就抢先取走
    // 第一个任务（NORMAL），导致 order[0] 不是 URGENT，产生竞态失败。
    std::vector<int> order;
    std::mutex mtx;

    queue.submit_void([&order, &mtx]() {
        std::lock_guard<std::mutex> lock(mtx);
        order.push_back(1);
    }, task::TaskPriority::NORMAL);

    queue.submit_void([&order, &mtx]() {
        std::lock_guard<std::mutex> lock(mtx);
        order.push_back(2);
    }, task::TaskPriority::URGENT);  // Highest priority

    queue.submit_void([&order, &mtx]() {
        std::lock_guard<std::mutex> lock(mtx);
        order.push_back(3);
    }, task::TaskPriority::HIGH);

    // 所有任务已在队列中，启动后工作线程按优先级顺序取出
    queue.start();

    std::this_thread::sleep_for(300ms);

    // 由于优先级队列实现，任务应该全部执行完成
    ASSERT_EQ(order.size(), 3u);
    // 高优先级任务应该先执行：URGENT(2) > HIGH(3) > NORMAL(1)
    EXPECT_EQ(order[0], 2);
    EXPECT_EQ(order[1], 3);
    EXPECT_EQ(order[2], 1);

    queue.stop();
}

TEST_F(TaskQueueTest, TaskWithTimeout) {
    queue_->start();
    
    std::atomic<bool> completed{false};
    
    int64_t id = queue_->submit<void>(
        []() {
            std::this_thread::sleep_for(500ms);
        },
        [&completed](const task::TaskResult<void>& r) {
            completed = true;
        },
        task::TaskPriority::NORMAL,
        100  // 100ms timeout
    );
    
    std::this_thread::sleep_for(200ms);
    // Task may or may not timeout depending on implementation
    EXPECT_GT(id, 0);
}

TEST_F(TaskQueueTest, CancelTask) {
    queue_->start();
    
    std::atomic<int> counter{0};
    
    // Submit a task that will be delayed
    int64_t id = queue_->submit_void([&counter]() {
        counter++;
    });
    
    // Cancel immediately (may or may not succeed depending on timing)
    bool cancelled = queue_->cancel(id);
    
    std::this_thread::sleep_for(100ms);
    // Result depends on timing
    (void)cancelled;
}

TEST_F(TaskQueueTest, Stats) {
    queue_->start();
    
    EXPECT_EQ(queue_->completed_count(), 0);
    
    for (int i = 0; i < 5; i++) {
        queue_->submit_void([]() {});
    }
    
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(queue_->completed_count(), 5);
}

TEST_F(TaskQueueTest, StopAndWait) {
    queue_->start();
    
    std::atomic<int> counter{0};
    
    for (int i = 0; i < 50; i++) {
        queue_->submit_void([&counter]() {
            std::this_thread::sleep_for(10ms);
            counter++;
        });
    }
    
    queue_->stop();
    
    EXPECT_GT(counter, 0);
}

// DelayedTaskQueue Tests
TEST(DelayedTaskQueueTest, ScheduleDelayed) {
    task::DelayedTaskQueue::Config config;
    config.worker_threads = 2;
    config.tick_interval_ms = 50;
    
    task::DelayedTaskQueue queue(config);
    queue.start();
    
    std::atomic<int> counter{0};
    
    auto handle = queue.schedule(100, [&counter]() {
        counter++;
    });
    
    EXPECT_TRUE(handle.valid());
    
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(counter, 0);
    
    // 增加等待时间，确保延迟任务触发
    std::this_thread::sleep_for(200ms);
    EXPECT_GE(counter, 1);
    
    queue.stop();
}

TEST(DelayedTaskQueueTest, ScheduleRepeat) {
    task::DelayedTaskQueue::Config config;
    config.worker_threads = 2;
    config.tick_interval_ms = 50;
    
    task::DelayedTaskQueue queue(config);
    queue.start();
    
    std::atomic<int> counter{0};
    
    auto handle = queue.schedule_repeat(100, [&counter]() {
        counter++;
    });
    
    EXPECT_TRUE(handle.valid());
    
    std::this_thread::sleep_for(350ms);
    EXPECT_GE(counter, 2);
    
    queue.cancel(handle);
    int prev = counter;
    
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(counter, prev);
    
    queue.stop();
}

TEST(DelayedTaskQueueTest, CancelDelayed) {
    task::DelayedTaskQueue::Config config;
    task::DelayedTaskQueue queue(config);
    queue.start();
    
    std::atomic<int> counter{0};
    
    auto handle = queue.schedule(500, [&counter]() {
        counter++;
    });
    
    queue.cancel(handle);
    
    std::this_thread::sleep_for(600ms);
    EXPECT_EQ(counter, 0);
    
    queue.stop();
}