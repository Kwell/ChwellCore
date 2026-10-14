#include "chwell/net/io_service.h"

namespace chwell { namespace net {

// IoService
void IoService::run() {
    while (!stopped_) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
            if (stopped_) break;
            if (queue_.empty()) continue;
            task = std::move(queue_.front());
            queue_.pop();
        }
        if (task) task();
    }
}

void IoService::stop() {
    {
        // Serialize the predicate change with run() entering its CV wait.
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    cv_.notify_all();
}

void IoService::post(std::function<void()> f) {
    if (!f) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(f));
    }
    cv_.notify_one();
}

} } // namespace chwell::net
