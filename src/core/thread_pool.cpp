#include "chwell/core/thread_pool.h"

namespace chwell {
namespace core {

ThreadPool::ThreadPool(std::size_t thread_count)
    : stopped_(false) {
    for (std::size_t i = 0; i < thread_count; ++i) {
        workers_.push_back(std::thread(&ThreadPool::worker_loop, this));
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    cond_.notify_all();
    for (std::size_t i = 0; i < workers_.size(); ++i) {
        if (workers_[i].joinable()) {
            workers_[i].join();
        }
    }
}

void ThreadPool::post(const std::function<void()>& task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return;
        }
        tasks_.push(task);
    }
    cond_.notify_one();
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this]() {
                return stopped_ || !tasks_.empty();
            });
            if (stopped_ && tasks_.empty()) {
                break;
            }
            task = tasks_.front();
            tasks_.pop();
        }

        if (task) {
            task();
        }
    }
}

} // namespace core
} // namespace chwell

