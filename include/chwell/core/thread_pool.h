#pragma once

#include <vector>
#include <thread>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace chwell {
namespace core {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void post(const std::function<void()>& task);

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()> > tasks_;
    std::mutex mutex_;
    std::condition_variable cond_;
    std::atomic<bool> stopped_;
};

} // namespace core
} // namespace chwell

