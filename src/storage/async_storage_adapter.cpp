#include "chwell/storage/async_storage_adapter.h"

namespace chwell {
namespace storage {

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

AsyncStorageAdapter::AsyncStorageAdapter(StorageInterface* storage,
                                         std::size_t num_threads)
    : storage_(storage) {
    workers_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this]() { worker_loop(); });
    }
}

AsyncStorageAdapter::~AsyncStorageAdapter() {
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        stopping_.store(true);
    }
    queue_cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

// ---------------------------------------------------------------------------
// 内部：工作线程循环
// ---------------------------------------------------------------------------

void AsyncStorageAdapter::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(queue_mutex_);
            queue_cv_.wait(lk,
                [this]() { return stopping_.load() || !tasks_.empty(); });
            if (tasks_.empty()) return;  // stopping == true 且队列为空
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

void AsyncStorageAdapter::enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        tasks_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// Future 风格
// ---------------------------------------------------------------------------

std::future<StorageResult> AsyncStorageAdapter::async_get(
    const std::string& key) {
    auto p = std::make_shared<std::promise<StorageResult>>();
    auto f = p->get_future();
    enqueue([this, key, p]() {
        p->set_value(storage_->get(key));
    });
    return f;
}

std::future<StorageResult> AsyncStorageAdapter::async_put(
    const std::string& key, const std::string& value,
    std::int64_t expire_at) {
    auto p = std::make_shared<std::promise<StorageResult>>();
    auto f = p->get_future();
    enqueue([this, key, value, expire_at, p]() {
        p->set_value(storage_->put(key, value, expire_at));
    });
    return f;
}

std::future<StorageResult> AsyncStorageAdapter::async_remove(
    const std::string& key) {
    auto p = std::make_shared<std::promise<StorageResult>>();
    auto f = p->get_future();
    enqueue([this, key, p]() {
        p->set_value(storage_->remove(key));
    });
    return f;
}

std::future<bool> AsyncStorageAdapter::async_exists(
    const std::string& key) {
    auto p = std::make_shared<std::promise<bool>>();
    auto f = p->get_future();
    enqueue([this, key, p]() {
        p->set_value(storage_->exists(key));
    });
    return f;
}

std::future<std::vector<StorageResult>> AsyncStorageAdapter::async_mget(
    const std::vector<std::string>& keys) {
    auto p = std::make_shared<std::promise<std::vector<StorageResult>>>();
    auto f = p->get_future();
    enqueue([this, keys, p]() {
        p->set_value(storage_->mget(keys));
    });
    return f;
}

std::future<StorageResult> AsyncStorageAdapter::async_mput(
    const std::vector<StorageDocument>& docs) {
    auto p = std::make_shared<std::promise<StorageResult>>();
    auto f = p->get_future();
    enqueue([this, docs, p]() {
        p->set_value(storage_->mput(docs));
    });
    return f;
}

// ---------------------------------------------------------------------------
// Callback 风格
// ---------------------------------------------------------------------------

void AsyncStorageAdapter::async_get(const std::string& key,
                                    AsyncCallback cb) {
    enqueue([this, key, cb]() {
        cb(storage_->get(key));
    });
}

void AsyncStorageAdapter::async_put(const std::string& key,
                                    const std::string& value,
                                    AsyncCallback cb,
                                    std::int64_t expire_at) {
    enqueue([this, key, value, expire_at, cb]() {
        cb(storage_->put(key, value, expire_at));
    });
}

void AsyncStorageAdapter::async_remove(const std::string& key,
                                       AsyncCallback cb) {
    enqueue([this, key, cb]() {
        cb(storage_->remove(key));
    });
}

void AsyncStorageAdapter::async_exists(const std::string& key,
                                       AsyncExistsCallback cb) {
    enqueue([this, key, cb]() {
        cb(storage_->exists(key));
    });
}

}  // namespace storage
}  // namespace chwell
