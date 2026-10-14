#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>

namespace chwell { namespace net {

// One-shot worker queue, independent of sockets and operating-system headers.
// stop() wakes every runner; a running callback finishes on its own thread.
// Queued callbacks are not drained after stop. Join runners before destruction.
class IoService {
public:
    IoService() = default;
    ~IoService() = default;
    void run();
    void stop();
    void post(std::function<void()> f);
private:
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> queue_;
};

} } // namespace chwell::net
