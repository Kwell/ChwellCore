#pragma once

#include <stack>
#include <memory>
#include <mutex>
#include <functional>
#include <atomic>

#include "chwell/core/logger.h"

namespace chwell {
namespace pool {

// 对象池配置
template<typename T>
struct ObjectPoolConfig {
    int initial_size;       // 初始对象数
    int max_size;           // 最大对象数
    int expand_size;        // 扩容时新增数量
    bool thread_safe;       // 是否线程安全
    
    ObjectPoolConfig()
        : initial_size(10), max_size(1000), 
          expand_size(10), thread_safe(true) {}
};

// 对象工厂
template<typename T>
struct ObjectFactory {
    std::function<std::unique_ptr<T>()> create;
    std::function<void(T*)> reset;      // 重置对象状态
    std::function<void(T*)> destroy;    // 销毁前回调
    
    ObjectFactory()
        : create([]() { return std::make_unique<T>(); })
        , reset(nullptr)
        , destroy(nullptr) {}
};

// 对象池
template<typename T>
class ObjectPool {
public:
    using Config = ObjectPoolConfig<T>;
    using Factory = ObjectFactory<T>;
    using Ptr = std::shared_ptr<ObjectPool<T>>;
    
    explicit ObjectPool(const Config& config = Config(), const Factory& factory = Factory())
        : config_(config)
        , factory_(factory)
        , created_count_(0)
        , borrowed_count_(0) {
        
        // 预创建对象
        for (int i = 0; i < config_.initial_size; ++i) {
            auto obj = factory_.create();
            if (obj) {
                pool_.push(std::move(obj));
                ++created_count_;
            }
        }
    }
    
    ~ObjectPool() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pool_.empty()) {
            auto obj = std::move(pool_.top());
            pool_.pop();
            if (factory_.destroy) {
                factory_.destroy(obj.get());
            }
        }
    }
    
    // 获取对象（返回智能指针，自动归还）
    std::unique_ptr<T, std::function<void(T*)>> acquire() {
        std::unique_ptr<T> obj;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            if (!pool_.empty()) {
                obj = std::move(pool_.top());
                pool_.pop();
            } else if (created_count_ < config_.max_size) {
                // 创建新对象
                obj = factory_.create();
                if (obj) {
                    ++created_count_;
                }
            }
        }
        
        if (!obj) {
            CHWELL_LOG_WARN("ObjectPool exhausted, created=" << created_count_ 
                          << ", max=" << config_.max_size);
            return std::unique_ptr<T, std::function<void(T*)>>(nullptr, nullptr);
        }
        
        ++borrowed_count_;
        
        // 返回带自定义删除器的智能指针
        return std::unique_ptr<T, std::function<void(T*)>>(
            obj.release(),
            [this](T* ptr) {
                this->release(ptr);
            }
        );
    }
    
    // 原始指针版本（需要手动归还）
    T* acquire_raw() {
        std::unique_ptr<T> obj;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            if (!pool_.empty()) {
                obj = std::move(pool_.top());
                pool_.pop();
            } else if (created_count_ < config_.max_size) {
                obj = factory_.create();
                if (obj) {
                    ++created_count_;
                }
            }
        }
        
        if (!obj) {
            return nullptr;
        }
        
        ++borrowed_count_;
        return obj.release();
    }
    
    // 归还对象（原始指针版本）
    void release(T* obj) {
        if (!obj) return;
        
        // 重置对象状态
        if (factory_.reset) {
            factory_.reset(obj);
        }
        
        std::unique_ptr<T> ptr(obj);
        
        std::lock_guard<std::mutex> lock(mutex_);
        --borrowed_count_;
        
        if (static_cast<int>(pool_.size()) < config_.max_size) {
            pool_.push(std::move(ptr));
        } else {
            // 池已满，销毁对象
            if (factory_.destroy) {
                factory_.destroy(ptr.get());
            }
            --created_count_;
        }
    }
    
    // 统计
    int created_count() const { return created_count_.load(); }
    int borrowed_count() const { return borrowed_count_.load(); }
    int available_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(pool_.size());
    }
    
    // 扩容
    void expand(int count) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        for (int i = 0; i < count && created_count_ < config_.max_size; ++i) {
            auto obj = factory_.create();
            if (obj) {
                pool_.push(std::move(obj));
                ++created_count_;
            }
        }
    }
    
    // 清理
    void shrink(int target_size) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        while (static_cast<int>(pool_.size()) > target_size) {
            auto obj = std::move(pool_.top());
            pool_.pop();
            if (factory_.destroy) {
                factory_.destroy(obj.get());
            }
            --created_count_;
        }
    }
    
private:
    Config config_;
    Factory factory_;
    
    mutable std::mutex mutex_;
    std::stack<std::unique_ptr<T>> pool_;
    
    std::atomic<int> created_count_;
    std::atomic<int> borrowed_count_;
};

// 缓冲区池（常用场景）
class BufferPool {
public:
    using Ptr = std::shared_ptr<BufferPool>;
    
    explicit BufferPool(int buffer_size = 4096, int initial_count = 10, int max_count = 1000)
        : buffer_size_(buffer_size)
        , max_count_(max_count)
        , created_count_(0)
        , borrowed_count_(0) {
        
        // 预创建
        for (int i = 0; i < initial_count && created_count_ < max_count; ++i) {
            pool_.push(std::make_unique<std::vector<char>>(buffer_size));
            ++created_count_;
        }
    }
    
    // 获取缓冲区
    std::unique_ptr<std::vector<char>> acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (!pool_.empty()) {
            auto buf = std::move(pool_.top());
            pool_.pop();
            buf->clear();
            ++borrowed_count_;
            return buf;
        }
        
        if (created_count_ < max_count_) {
            ++created_count_;
            ++borrowed_count_;
            return std::make_unique<std::vector<char>>(buffer_size_);
        }
        
        return nullptr;
    }
    
    // 归还缓冲区
    void release(std::unique_ptr<std::vector<char>> buf) {
        if (!buf) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        --borrowed_count_;
        
        if (static_cast<int>(pool_.size()) < max_count_) {
            buf->clear();  // 清空但保留容量
            pool_.push(std::move(buf));
        } else {
            --created_count_;
        }
    }
    
    int buffer_size() const { return buffer_size_; }
    int created_count() const { return created_count_; }
    int available_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(pool_.size());
    }
    
private:
    int buffer_size_;
    int max_count_;
    
    mutable std::mutex mutex_;
    std::stack<std::unique_ptr<std::vector<char>>> pool_;
    
    std::atomic<int> created_count_;
    std::atomic<int> borrowed_count_;
};

// 全局缓冲区池
class GlobalBufferPool {
public:
    static BufferPool::Ptr get_pool(int size = 4096) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = pools_.find(size);
        if (it != pools_.end()) {
            return it->second;
        }
        
        auto pool = std::make_shared<BufferPool>(size, 10, 1000);
        pools_[size] = pool;
        return pool;
    }
    
    // 快捷方法
    static std::unique_ptr<std::vector<char>> acquire(int size = 4096) {
        return get_pool(size)->acquire();
    }
    
    static void release(std::unique_ptr<std::vector<char>> buf, int size = 4096) {
        get_pool(size)->release(std::move(buf));
    }
    
private:
    static std::mutex mutex_;
    static std::unordered_map<int, BufferPool::Ptr> pools_;
};

// 常用对象池类型定义
using MessageBufferPool = BufferPool;

} // namespace pool
} // namespace chwell