#include "chwell/task/task_queue.h"
#include <algorithm>

namespace chwell {
namespace task {

//=============================================================================
// TaskQueue 实现
//=============================================================================

TaskQueue::TaskQueue(const Config& config)
    : config_(config)
    , running_(false)
    , next_task_id_(1)
    , running_count_(0)
    , completed_count_(0) {
}

TaskQueue::~TaskQueue() {
    stop();
}

void TaskQueue::start() {
    if (running_.exchange(true)) {
        CHWELL_LOG_WARN("TaskQueue already running");
        return; // 已经在运行
    }

    CHWELL_LOG_INFO("TaskQueue starting with " << config_.worker_threads << " worker threads");

    for (int i = 0; i < config_.worker_threads; ++i) {
        workers_.emplace_back([this]() {
            worker_loop();
        });
    }

    CHWELL_LOG_INFO("TaskQueue started with " << config_.worker_threads << " workers");
}

void TaskQueue::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    
    cv_.notify_all();
    
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    
    CHWELL_LOG_INFO("TaskQueue stopped, completed=" << completed_count_.load());
}

void TaskQueue::worker_loop() {
    while (running_) {
        std::shared_ptr<TaskBase> task;
        
        {
            std::unique_lock<std::mutex> lock(mutex_);
            
            cv_.wait(lock, [this]() {
                return !running_ || !queue_.empty();
            });
            
            if (!running_ && queue_.empty()) {
                break;
            }
            
            if (!queue_.empty()) {
                task = queue_.top().second;
                queue_.pop();
            }
        }
        
        if (task) {
            ++running_count_;
            
            try {
                task->execute();
            } catch (const std::exception& e) {
                CHWELL_LOG_ERROR("Task execution exception: " << e.what());
            }
            
            --running_count_;
            ++completed_count_;
            
            // 从任务列表移除
            {
                std::lock_guard<std::mutex> lock(mutex_);
                tasks_.erase(task->id());
            }
        }
    }
}

int64_t TaskQueue::submit_void(std::function<void()> func, TaskPriority priority) {
    return submit<bool>([func]() -> bool {
        func();
        return true;
    }, nullptr, priority);
}

bool TaskQueue::cancel(int64_t task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
        // 注意：只能取消还在队列中的任务
        tasks_.erase(it);
        return true;
    }
    
    return false;
}

void TaskQueue::wait_all() {
    while (pending_count() > 0 || running_count() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int TaskQueue::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(queue_.size());
}

int TaskQueue::running_count() const {
    return running_count_.load();
}

int64_t TaskQueue::completed_count() const {
    return completed_count_.load();
}

//=============================================================================
// DelayedTaskQueue 实现
//=============================================================================

DelayedTaskQueue::DelayedTaskQueue(const Config& config)
    : config_(config)
    , task_queue_(TaskQueue::Config()) {
}

DelayedTaskQueue::~DelayedTaskQueue() {
    stop();
}

void DelayedTaskQueue::start() {
    timer_wheel_ = std::make_unique<core::TimerWheel>(
        config_.tick_interval_ms, 60, 4);
    timer_wheel_->start();
    
    task_queue_.start();
    
    CHWELL_LOG_INFO("DelayedTaskQueue started");
}

void DelayedTaskQueue::stop() {
    if (timer_wheel_) {
        timer_wheel_->stop();
        timer_wheel_.reset();
    }
    
    task_queue_.stop();
    
    CHWELL_LOG_INFO("DelayedTaskQueue stopped");
}

core::TimerHandle DelayedTaskQueue::schedule(int delay_ms, std::function<void()> task) {
    if (!timer_wheel_) {
        return core::TimerHandle();
    }
    
    return timer_wheel_->add_timer(delay_ms, [this, task]() {
        task_queue_.submit_void(task, task::TaskPriority::NORMAL);
    });
}

core::TimerHandle DelayedTaskQueue::schedule_repeat(int interval_ms, std::function<void()> task) {
    if (!timer_wheel_) {
        return core::TimerHandle();
    }
    
    return timer_wheel_->add_repeat_timer(interval_ms, [this, task]() {
        task_queue_.submit_void(task, task::TaskPriority::NORMAL);
    });
}

void DelayedTaskQueue::cancel(core::TimerHandle& handle) {
    if (timer_wheel_) {
        timer_wheel_->cancel_timer(handle);
    }
}

int DelayedTaskQueue::pending_count() const {
    return task_queue_.pending_count() + 
           (timer_wheel_ ? 0 : 0);  // TimerWheel 没有暴露 pending count
}

} // namespace task
} // namespace chwell