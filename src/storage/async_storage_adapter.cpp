#include "chwell/storage/async_storage_adapter.h"

#include <exception>
#include <vector>

namespace chwell {
namespace storage {

namespace {

constexpr const char kShutdownMsg[] =
    "AsyncStorageAdapter is shutting down";
constexpr const char kNullStorageMsg[] =
    "AsyncStorageAdapter: storage is null";

template <typename F>
void fulfill_result_promise(const std::shared_ptr<std::promise<StorageResult>>& p,
                            F&& f) {
    try {
        p->set_value(f());
    } catch (const std::exception& e) {
        p->set_value(StorageResult::failure(e.what()));
    } catch (...) {
        p->set_value(StorageResult::failure("unknown exception"));
    }
}

template <typename F>
void fulfill_bool_promise(const std::shared_ptr<std::promise<bool>>& p,
                          F&& f) {
    try {
        p->set_value(f());
    } catch (...) {
        p->set_value(false);
    }
}

template <typename F>
void fulfill_vector_promise(
    const std::shared_ptr<std::promise<std::vector<StorageResult>>>& p,
    F&& f) {
    try {
        p->set_value(f());
    } catch (const std::exception& e) {
        p->set_value(std::vector<StorageResult>{
            StorageResult::failure(e.what())});
    } catch (...) {
        p->set_value(std::vector<StorageResult>{
            StorageResult::failure("unknown exception")});
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

AsyncStorageAdapter::AsyncStorageAdapter(StorageInterface* storage,
                                         std::size_t num_threads,
                                         std::size_t max_queue_size)
    : storage_(storage), max_queue_size_(max_queue_size) {
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
            queue_cv_.wait(lk, [this]() {
                return stopping_.load() || !tasks_.empty();
            });
            if (tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        queue_cv_.notify_one();
        task();
    }
}

bool AsyncStorageAdapter::enqueue(std::function<void()> task) {
    std::unique_lock<std::mutex> lk(queue_mutex_);
    if (stopping_.load()) {
        return false;
    }
    if (max_queue_size_ > 0) {
        queue_cv_.wait(lk, [this] {
            return stopping_.load() || tasks_.size() < max_queue_size_;
        });
        if (stopping_.load()) {
            return false;
        }
    }
    tasks_.push(std::move(task));
    lk.unlock();
    queue_cv_.notify_one();
    return true;
}

// ---------------------------------------------------------------------------
// Future 风格
// ---------------------------------------------------------------------------

std::future<StorageResult> AsyncStorageAdapter::async_get(
    const std::string& key) {
    auto p = std::make_shared<std::promise<StorageResult>>();
    auto f = p->get_future();
    if (!storage_) {
        p->set_value(StorageResult::failure(kNullStorageMsg));
        return f;
    }
    if (!enqueue([this, key, p]() {
            fulfill_result_promise(p, [this, &key] {
                return storage_->get(key);
            });
        })) {
        p->set_value(StorageResult::failure(kShutdownMsg));
    }
    return f;
}

std::future<StorageResult> AsyncStorageAdapter::async_put(
    const std::string& key, const std::string& value,
    std::int64_t expire_at) {
    auto p = std::make_shared<std::promise<StorageResult>>();
    auto f = p->get_future();
    if (!storage_) {
        p->set_value(StorageResult::failure(kNullStorageMsg));
        return f;
    }
    std::string val(value);
    if (!enqueue([this, key, val = std::move(val), expire_at, p]() {
            fulfill_result_promise(p, [this, &key, &val, expire_at] {
                return storage_->put(key, val, expire_at);
            });
        })) {
        p->set_value(StorageResult::failure(kShutdownMsg));
    }
    return f;
}

std::future<StorageResult> AsyncStorageAdapter::async_remove(
    const std::string& key) {
    auto p = std::make_shared<std::promise<StorageResult>>();
    auto f = p->get_future();
    if (!storage_) {
        p->set_value(StorageResult::failure(kNullStorageMsg));
        return f;
    }
    if (!enqueue([this, key, p]() {
            fulfill_result_promise(p, [this, &key] {
                return storage_->remove(key);
            });
        })) {
        p->set_value(StorageResult::failure(kShutdownMsg));
    }
    return f;
}

std::future<bool> AsyncStorageAdapter::async_exists(
    const std::string& key) {
    auto p = std::make_shared<std::promise<bool>>();
    auto f = p->get_future();
    if (!storage_) {
        p->set_value(false);
        return f;
    }
    if (!enqueue([this, key, p]() {
            fulfill_bool_promise(p, [this, &key] {
                return storage_->exists(key);
            });
        })) {
        p->set_value(false);
    }
    return f;
}

std::future<std::vector<StorageResult>> AsyncStorageAdapter::async_mget(
    const std::vector<std::string>& keys) {
    auto p = std::make_shared<std::promise<std::vector<StorageResult>>>();
    auto f = p->get_future();
    auto fail_vec = [&keys](const char* msg) {
        std::vector<StorageResult> bad;
        bad.reserve(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) {
            bad.push_back(StorageResult::failure(msg));
        }
        return bad;
    };
    if (!storage_) {
        p->set_value(fail_vec(kNullStorageMsg));
        return f;
    }
    std::vector<std::string> keys_owned(keys);
    if (!enqueue([this, keys = std::move(keys_owned), p]() {
            fulfill_vector_promise(p, [this, &keys] {
                return storage_->mget(keys);
            });
        })) {
        p->set_value(fail_vec(kShutdownMsg));
    }
    return f;
}

std::future<StorageResult> AsyncStorageAdapter::async_mput(
    const std::vector<StorageDocument>& docs) {
    auto p = std::make_shared<std::promise<StorageResult>>();
    auto f = p->get_future();
    if (!storage_) {
        p->set_value(StorageResult::failure(kNullStorageMsg));
        return f;
    }
    std::vector<StorageDocument> docs_owned(docs);
    if (!enqueue([this, docs = std::move(docs_owned), p]() {
            fulfill_result_promise(p, [this, &docs] {
                return storage_->mput(docs);
            });
        })) {
        p->set_value(StorageResult::failure(kShutdownMsg));
    }
    return f;
}

// ---------------------------------------------------------------------------
// Callback 风格
// ---------------------------------------------------------------------------

void AsyncStorageAdapter::async_get(const std::string& key,
                                    AsyncCallback cb) {
    if (!storage_) {
        cb(StorageResult::failure(kNullStorageMsg));
        return;
    }
    if (!enqueue([this, key, cb]() {
            try {
                cb(storage_->get(key));
            } catch (const std::exception& e) {
                cb(StorageResult::failure(e.what()));
            } catch (...) {
                cb(StorageResult::failure("unknown exception"));
            }
        })) {
        cb(StorageResult::failure(kShutdownMsg));
    }
}

void AsyncStorageAdapter::async_put(const std::string& key,
                                    const std::string& value,
                                    AsyncCallback cb,
                                    std::int64_t expire_at) {
    if (!storage_) {
        cb(StorageResult::failure(kNullStorageMsg));
        return;
    }
    std::string val(value);
    if (!enqueue([this, key, val = std::move(val), expire_at, cb]() {
            try {
                cb(storage_->put(key, val, expire_at));
            } catch (const std::exception& e) {
                cb(StorageResult::failure(e.what()));
            } catch (...) {
                cb(StorageResult::failure("unknown exception"));
            }
        })) {
        cb(StorageResult::failure(kShutdownMsg));
    }
}

void AsyncStorageAdapter::async_remove(const std::string& key,
                                       AsyncCallback cb) {
    if (!storage_) {
        cb(StorageResult::failure(kNullStorageMsg));
        return;
    }
    if (!enqueue([this, key, cb]() {
            try {
                cb(storage_->remove(key));
            } catch (const std::exception& e) {
                cb(StorageResult::failure(e.what()));
            } catch (...) {
                cb(StorageResult::failure("unknown exception"));
            }
        })) {
        cb(StorageResult::failure(kShutdownMsg));
    }
}

void AsyncStorageAdapter::async_exists(const std::string& key,
                                       AsyncExistsCallback cb) {
    if (!storage_) {
        cb(false);
        return;
    }
    if (!enqueue([this, key, cb]() {
            try {
                cb(storage_->exists(key));
            } catch (...) {
                cb(false);
            }
        })) {
        cb(false);
    }
}

}  // namespace storage
}  // namespace chwell
