#pragma once

#include <queue>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

#include "chwell/core/logger.h"
#include "chwell/core/timer_wheel.h"

namespace chwell {
namespace task {

// 任务优先级
enum class TaskPriority {
    LOW = 0,
    NORMAL = 1,
    HIGH = 2,
    URGENT = 3
};

// 任务状态
enum class TaskStatus {
    PENDING,        // 等待执行
    RUNNING,        // 正在执行
    COMPLETED,      // 已完成
    FAILED,         // 失败
    CANCELLED,      // 已取消
    TIMEOUT         // 超时
};

// 任务结果
template<typename T>
struct TaskResult {
    TaskStatus status;
    T value;
    std::string error;
    
    TaskResult() : status(TaskStatus::PENDING) {}
    
    bool ok() const { return status == TaskStatus::COMPLETED; }
    bool failed() const { return status == TaskStatus::FAILED || 
                                status == TaskStatus::TIMEOUT ||
                                status == TaskStatus::CANCELLED; }
};

// void 特化
template<>
struct TaskResult<void> {
    TaskStatus status;
    std::string error;
    
    TaskResult() : status(TaskStatus::PENDING) {}
    
    bool ok() const { return status == TaskStatus::COMPLETED; }
    bool failed() const { return status == TaskStatus::FAILED || 
                                status == TaskStatus::TIMEOUT ||
                                status == TaskStatus::CANCELLED; }
};

// 任务基类
class TaskBase {
public:
    virtual ~TaskBase() = default;
    virtual void execute() = 0;
    
    TaskPriority priority() const { return priority_; }
    void set_priority(TaskPriority p) { priority_ = p; }
    
    int64_t id() const { return id_; }
    void set_id(int64_t id) { id_ = id; }
    
    int timeout_ms() const { return timeout_ms_; }
    void set_timeout(int ms) { timeout_ms_ = ms; }
    
    int retry_count() const { return retry_count_; }
    void set_retry_count(int count) { retry_count_ = count; }
    
    int retry_attempt() const { return retry_attempt_; }
    
protected:
    TaskPriority priority_ = TaskPriority::NORMAL;
    int64_t id_ = 0;
    int timeout_ms_ = 0;
    int retry_count_ = 0;
    int retry_attempt_ = 0;
};

// 泛型任务
template<typename T>
class Task : public TaskBase {
public:
    using Callback = std::function<void(const TaskResult<T>&)>;
    using TaskFunc = std::function<T()>;
    
    Task(TaskFunc func, Callback callback = nullptr)
        : func_(std::move(func))
        , callback_(std::move(callback)) {}
    
    void execute() override {
        result_.status = TaskStatus::RUNNING;
        
        try {
            result_.value = func_();
            result_.status = TaskStatus::COMPLETED;
        } catch (const std::exception& e) {
            result_.status = TaskStatus::FAILED;
            result_.error = e.what();
        } catch (...) {
            result_.status = TaskStatus::FAILED;
            result_.error = "Unknown error";
        }
        
        if (callback_) {
            callback_(result_);
        }
    }
    
    const TaskResult<T>& result() const { return result_; }
    
private:
    TaskFunc func_;
    Callback callback_;
    TaskResult<T> result_;
};

// void 任务特化
template<>
class Task<void> : public TaskBase {
public:
    using Callback = std::function<void(const TaskResult<void>&)>;
    using TaskFunc = std::function<void()>;
    
    Task(TaskFunc func, Callback callback = nullptr)
        : func_(std::move(func))
        , callback_(std::move(callback)) {}
    
    void execute() override {
        result_.status = TaskStatus::RUNNING;
        
        try {
            func_();
            result_.status = TaskStatus::COMPLETED;
        } catch (const std::exception& e) {
            result_.status = TaskStatus::FAILED;
            result_.error = e.what();
        } catch (...) {
            result_.status = TaskStatus::FAILED;
            result_.error = "Unknown error";
        }
        
        if (callback_) {
            callback_(result_);
        }
    }
    
    const TaskResult<void>& result() const { return result_; }
    
private:
    TaskFunc func_;
    Callback callback_;
    TaskResult<void> result_;
};

// 无返回值任务
using VoidTask = Task<void>;

// 任务队列
class TaskQueue {
public:
    struct Config {
        int worker_threads;     // 工作线程数
        int max_queue_size;     // 最大队列大小
        bool enable_priority;   // 是否启用优先级
        
        Config()
            : worker_threads(4)
            , max_queue_size(10000)
            , enable_priority(true) {}
    };
    
    explicit TaskQueue(const Config& config = Config());
    ~TaskQueue();
    
    // 启动队列
    void start();
    
    // 停止队列
    void stop();
    
    // 提交任务
    template<typename T>
    int64_t submit(std::function<T()> func,
                   std::function<void(const TaskResult<T>&)> callback = nullptr,
                   TaskPriority priority = TaskPriority::NORMAL,
                   int timeout_ms = 0,
                   int retry_count = 0);
    
    // 提交简单任务（无返回值）
    int64_t submit_void(std::function<void()> func,
                        TaskPriority priority = TaskPriority::NORMAL);
    
    // 取消任务
    bool cancel(int64_t task_id);
    
    // 等待所有任务完成
    void wait_all();
    
    // 统计
    int pending_count() const;
    int running_count() const;
    int64_t completed_count() const;
    
private:
    void worker_loop();
    
    Config config_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    
    std::priority_queue<std::pair<int, std::shared_ptr<TaskBase>>> queue_;
    std::unordered_map<int64_t, std::shared_ptr<TaskBase>> tasks_;
    std::vector<std::thread> workers_;
    
    std::atomic<bool> running_;
    std::atomic<int64_t> next_task_id_;
    std::atomic<int> running_count_;
    std::atomic<int64_t> completed_count_;
};

// 延迟任务队列
class DelayedTaskQueue {
public:
    struct Config {
        int worker_threads;
        int tick_interval_ms;
        
        Config()
            : worker_threads(2)
            , tick_interval_ms(100) {}
    };
    
    explicit DelayedTaskQueue(const Config& config = Config());
    ~DelayedTaskQueue();
    
    void start();
    void stop();
    
    // 添加延迟任务
    core::TimerHandle schedule(int delay_ms, std::function<void()> task);
    
    // 添加重复任务
    core::TimerHandle schedule_repeat(int interval_ms, std::function<void()> task);
    
    // 取消任务
    void cancel(core::TimerHandle& handle);
    
    int pending_count() const;
    
private:
    Config config_;
    std::unique_ptr<core::TimerWheel> timer_wheel_;
    TaskQueue task_queue_;
};

//=============================================================================
// 实现部分
//=============================================================================

template<typename T>
int64_t TaskQueue::submit(std::function<T()> func,
                          std::function<void(const TaskResult<T>&)> callback,
                          TaskPriority priority,
                          int timeout_ms,
                          int retry_count) {
    auto task = std::make_shared<Task<T>>(std::move(func), std::move(callback));
    task->set_priority(priority);
    task->set_timeout(timeout_ms);
    task->set_retry_count(retry_count);
    
    int64_t id = next_task_id_.fetch_add(1);
    task->set_id(id);
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (static_cast<int>(queue_.size()) >= config_.max_queue_size) {
        CHWELL_LOG_WARN("TaskQueue full, rejecting task " << id);
        return -1;
    }
    
    tasks_[id] = task;
    
    // 优先级越高，数值越大（优先队列默认大顶堆）
    int priority_value = config_.enable_priority ? 
                         static_cast<int>(priority) : 0;
    queue_.emplace(priority_value, task);
    
    cv_.notify_one();
    
    return id;
}

} // namespace task
} // namespace chwell