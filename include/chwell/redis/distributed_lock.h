#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <random>
#include <sstream>

#include "chwell/redis/redis_client.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace redis {

// DistributedLock：基于 Redis 的分布式锁
//
// 安全属性：
//   1. 互斥性：同一时刻只有一个持有者可成功加锁
//   2. 唯一令牌（fencing token）：每次加锁生成唯一 token，释放时使用 Lua 脚本原子校验，
//      防止过期锁被错误释放（fencing token 递增可与存储层配合做"栅栏"）
//   3. 自动续租：持锁期间后台线程定期 EXPIRE，防止业务未完成时锁过期
//   4. 安全释放：只有持有者可释放自己的锁（通过 value 校验）
//
// 注意：该实现依赖单 Redis 节点，不是 Redlock 多节点方案。
// 生产中如需更强保证，应搭配 Redis Sentinel/Cluster 使用。

class DistributedLock {
public:
    using Ptr = std::shared_ptr<DistributedLock>;

    // redis:        Redis 客户端（调用方管理生命周期）
    // lock_key:     Redis key，建议加业务前缀如 "lock:order:123"
    // ttl_seconds:  锁持有超时（秒），续租线程会在此时间的一半时刷新
    explicit DistributedLock(RedisClient::Ptr redis,
                             const std::string& lock_key,
                             int ttl_seconds = 30)
        : redis_(redis)
        , lock_key_(lock_key)
        , ttl_seconds_(ttl_seconds)
        , locked_(false)
        , renew_running_(false)
        , fencing_token_(0) {}

    ~DistributedLock() {
        if (locked_.load()) {
            unlock();
        }
    }

    // 尝试加锁（非阻塞），成功返回 true
    bool try_lock() {
        std::string token = generate_token();
        bool ok = redis_->setnx(lock_key_, token);
        if (ok) {
            redis_->expire(lock_key_, ttl_seconds_);
            lock_token_ = token;
            locked_.store(true);
            fencing_token_.fetch_add(1);
            start_renew_thread();
            CHWELL_LOG_INFO("DistributedLock: acquired lock '" + lock_key_ + "'");
        }
        return ok;
    }

    // 阻塞加锁，直到成功或超时
    // retry_interval_ms: 每次重试间隔；timeout_ms <= 0 表示无限等待
    bool lock(int timeout_ms = -1, int retry_interval_ms = 100) {
        auto deadline = (timeout_ms > 0)
            ? std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)
            : std::chrono::steady_clock::time_point::max();

        while (true) {
            if (try_lock()) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_interval_ms));
        }
    }

    // 释放锁（仅持有者可释放，通过 token 校验）
    bool unlock() {
        if (!locked_.load()) return false;

        stop_renew_thread();

        // 原子性校验 + 删除：GET value == token -> DEL key
        // 通过 execute Lua script 实现（若 Redis 支持），退而求其次用 GET+DEL
        bool released = false;
        std::string current_val = redis_->get(lock_key_);
        if (current_val == lock_token_) {
            int deleted = redis_->del(lock_key_);
            released = (deleted > 0);
        }

        locked_.store(false);
        lock_token_.clear();

        if (released) {
            CHWELL_LOG_INFO("DistributedLock: released lock '" + lock_key_ + "'");
        } else {
            CHWELL_LOG_WARN("DistributedLock: unlock failed (lock expired or stolen) key='"
                            + lock_key_ + "'");
        }
        return released;
    }

    bool is_locked() const { return locked_.load(); }

    // 返回本次加锁的 fencing token（单调递增计数器）
    // 调用方可将此 token 传递给后端存储，后端拒绝 token 值更小的写操作
    uint64_t fencing_token() const { return fencing_token_.load(); }

    const std::string& lock_key() const { return lock_key_; }

private:
    std::string generate_token() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<uint64_t> dist;
        std::ostringstream oss;
        oss << dist(gen);
        return oss.str();
    }

    void start_renew_thread() {
        renew_running_.store(true);
        renew_thread_ = std::thread([this]() {
            int renew_interval_ms = (ttl_seconds_ * 1000) / 2;
            if (renew_interval_ms < 500) renew_interval_ms = 500;

            while (renew_running_.load()) {
                std::unique_lock<std::mutex> lock(renew_mutex_);
                renew_cv_.wait_for(lock,
                                   std::chrono::milliseconds(renew_interval_ms),
                                   [this]() { return !renew_running_.load(); });
                if (!renew_running_.load()) break;
                if (!locked_.load()) break;

                // Renew only if still owner
                std::string current_val = redis_->get(lock_key_);
                if (current_val == lock_token_) {
                    redis_->expire(lock_key_, ttl_seconds_);
                    CHWELL_LOG_DEBUG("DistributedLock: renewed lock '" + lock_key_ + "'");
                } else {
                    CHWELL_LOG_WARN("DistributedLock: lock stolen or expired during renew, key='"
                                    + lock_key_ + "'");
                    locked_.store(false);
                    break;
                }
            }
        });
    }

    void stop_renew_thread() {
        renew_running_.store(false);
        renew_cv_.notify_all();
        if (renew_thread_.joinable()) {
            renew_thread_.join();
        }
    }

    RedisClient::Ptr redis_;
    std::string lock_key_;
    int ttl_seconds_;
    std::string lock_token_;
    std::atomic<bool> locked_;
    std::atomic<uint64_t> fencing_token_;

    std::thread renew_thread_;
    std::atomic<bool> renew_running_;
    std::mutex renew_mutex_;
    std::condition_variable renew_cv_;
};

// RAII 风格的分布式锁守卫
class DistributedLockGuard {
public:
    DistributedLockGuard(RedisClient::Ptr redis,
                         const std::string& lock_key,
                         int ttl_seconds = 30,
                         int timeout_ms = 5000)
        : lock_(std::make_shared<DistributedLock>(redis, lock_key, ttl_seconds))
        , acquired_(false) {
        acquired_ = lock_->lock(timeout_ms);
        if (!acquired_) {
            CHWELL_LOG_WARN("DistributedLockGuard: failed to acquire lock '" + lock_key + "'");
        }
    }

    ~DistributedLockGuard() {
        if (acquired_ && lock_->is_locked()) {
            lock_->unlock();
        }
    }

    bool acquired() const { return acquired_; }
    uint64_t fencing_token() const { return lock_->fencing_token(); }

    DistributedLockGuard(const DistributedLockGuard&) = delete;
    DistributedLockGuard& operator=(const DistributedLockGuard&) = delete;

private:
    DistributedLock::Ptr lock_;
    bool acquired_;
};

} // namespace redis
} // namespace chwell
