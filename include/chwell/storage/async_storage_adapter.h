#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "chwell/storage/async_storage_interface.h"
#include "chwell/storage/storage_interface.h"

namespace chwell {
namespace storage {

// AsyncStorageAdapter：将任意同步 StorageInterface 包装为 AsyncStorageInterface。
//
// 内部维护固定大小的工作线程池（默认 4 线程）和任务队列，将同步 I/O 调用
// 提交到后台线程执行，调用方通过 std::future 或回调取得结果。
//
// 生命周期：
//   - 构造后自动启动工作线程
//   - 析构时等待所有已提交任务执行完毕后停止线程
//
// 线程安全：所有 async_* 方法均可从任意线程并发调用。
class AsyncStorageAdapter : public AsyncStorageInterface {
public:
    // storage 必须在 AsyncStorageAdapter 的整个生命周期内有效
    explicit AsyncStorageAdapter(StorageInterface* storage,
                                 std::size_t num_threads = 4);

    ~AsyncStorageAdapter() override;

    // 禁止拷贝
    AsyncStorageAdapter(const AsyncStorageAdapter&)            = delete;
    AsyncStorageAdapter& operator=(const AsyncStorageAdapter&) = delete;

    // -----------------------------------------------------------------------
    // Future 风格
    // -----------------------------------------------------------------------
    std::future<StorageResult> async_get(const std::string& key) override;
    std::future<StorageResult> async_put(const std::string& key,
                                         const std::string& value,
                                         std::int64_t expire_at = 0) override;
    std::future<StorageResult> async_remove(const std::string& key) override;
    std::future<bool>          async_exists(const std::string& key) override;
    std::future<std::vector<StorageResult>> async_mget(
        const std::vector<std::string>& keys) override;
    std::future<StorageResult> async_mput(
        const std::vector<StorageDocument>& docs) override;

    // -----------------------------------------------------------------------
    // Callback 风格
    // -----------------------------------------------------------------------
    void async_get(const std::string& key, AsyncCallback cb) override;
    void async_put(const std::string& key, const std::string& value,
                   AsyncCallback cb, std::int64_t expire_at = 0) override;
    void async_remove(const std::string& key, AsyncCallback cb) override;
    void async_exists(const std::string& key, AsyncExistsCallback cb) override;

private:
    // 向任务队列提交一个可调用对象
    void enqueue(std::function<void()> task);

    // 工作线程主循环
    void worker_loop();

    StorageInterface*              storage_;
    std::vector<std::thread>       workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                     queue_mutex_;
    std::condition_variable        queue_cv_;
    std::atomic<bool>              stopping_{false};
};

}  // namespace storage
}  // namespace chwell
