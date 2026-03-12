#pragma once

#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <atomic>
#include <unordered_map>
#include <thread>

#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace net {

// 连接池配置
struct ConnectionPoolConfig {
    std::string host;
    unsigned short port;
    int min_connections;        // 最小连接数
    int max_connections;        // 最大连接数
    int connect_timeout_ms;     // 连接超时（毫秒）
    int idle_timeout_ms;        // 空闲超时（毫秒）
    int max_lifetime_ms;        // 最大生命周期（毫秒），0表示无限
    
    ConnectionPoolConfig()
        : port(0), min_connections(2), max_connections(10),
          connect_timeout_ms(5000), idle_timeout_ms(300000),
          max_lifetime_ms(0) {}
};

// 池化的连接
struct PooledConnection {
    TcpConnectionPtr connection;
    int64_t create_time;        // 创建时间
    int64_t last_used_time;     // 最后使用时间
    bool in_use;                // 是否正在使用
    bool is_valid;              // 是否有效
    
    PooledConnection() 
        : create_time(0), last_used_time(0), 
          in_use(false), is_valid(false) {}
    
    static int64_t current_time_ms() {
        auto now = std::chrono::steady_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    }
};

// 连接池
class ConnectionPool : public std::enable_shared_from_this<ConnectionPool> {
public:
    using Ptr = std::shared_ptr<ConnectionPool>;
    using ConnectionCallback = std::function<void(const PooledConnection&)>;
    
    // 创建连接池
    static Ptr create(IoService& io_service, const ConnectionPoolConfig& config) {
        return Ptr(new ConnectionPool(io_service, config));
    }
    
    ~ConnectionPool();
    
    // 初始化连接池（创建最小连接数）
    bool init();
    
    // 关闭连接池
    void shutdown();
    
    // 获取连接（异步）
    // callback: 获取成功或失败的回调
    // timeout_ms: 等待超时，-1表示无限等待
    void get_connection(ConnectionCallback callback, int timeout_ms = 5000);
    
    // 获取连接（同步，阻塞）
    // 返回空指针表示失败
    PooledConnection get_connection_sync(int timeout_ms = 5000);
    
    // 归还连接
    void return_connection(PooledConnection& conn);
    
    // 获取统计信息
    int total_connections() const;
    int idle_connections() const;
    int active_connections() const;
    
    // 获取配置
    const ConnectionPoolConfig& config() const { return config_; }
    
private:
    ConnectionPool(IoService& io_service, const ConnectionPoolConfig& config);
    
    // 创建新连接
    bool create_connection(PooledConnection& conn);
    
    // 检查连接有效性
    bool validate_connection(PooledConnection& conn);
    
    // 清理过期连接
    void cleanup_expired();
    
    // 检查并扩容
    void maybe_expand();
    
    // 尝试获取空闲连接
    bool try_get_idle(PooledConnection& conn);
    
    IoService& io_service_;
    ConnectionPoolConfig config_;
    
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    
    std::vector<std::unique_ptr<PooledConnection>> connections_;
    std::queue<ConnectionCallback> waiting_callbacks_;
    
    std::atomic<bool> shutdown_;
    std::atomic<int> pending_creates_;  // 正在创建的连接数
};

// 连接池管理器：管理多个目标地址的连接池
class ConnectionPoolManager {
public:
    static ConnectionPoolManager& instance() {
        static ConnectionPoolManager inst;
        return inst;
    }
    
    // 创建或获取连接池
    ConnectionPool::Ptr get_pool(IoService& io_service, 
                                  const std::string& name,
                                  const ConnectionPoolConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = pools_.find(name);
        if (it != pools_.end()) {
            return it->second;
        }
        
        auto pool = ConnectionPool::create(io_service, config);
        if (pool && pool->init()) {
            pools_[name] = pool;
            return pool;
        }
        
        return nullptr;
    }
    
    // 获取已存在的连接池
    ConnectionPool::Ptr get_pool(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pools_.find(name);
        if (it != pools_.end()) {
            return it->second;
        }
        return nullptr;
    }
    
    // 移除连接池
    void remove_pool(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pools_.find(name);
        if (it != pools_.end()) {
            it->second->shutdown();
            pools_.erase(it);
        }
    }
    
    // 关闭所有连接池
    void shutdown_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& pair : pools_) {
            pair.second->shutdown();
        }
        pools_.clear();
    }
    
private:
    ConnectionPoolManager() = default;
    
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ConnectionPool::Ptr> pools_;
};

// RAII 风格的连接守卫
class ConnectionGuard {
public:
    ConnectionGuard() : pool_(nullptr) {}
    ConnectionGuard(ConnectionPool::Ptr pool, PooledConnection conn)
        : pool_(pool), conn_(conn) {}
    
    ConnectionGuard(ConnectionGuard&& other) noexcept
        : pool_(other.pool_), conn_(other.conn_) {
        other.pool_ = nullptr;
    }
    
    ConnectionGuard& operator=(ConnectionGuard&& other) noexcept {
        if (this != &other) {
            release();
            pool_ = other.pool_;
            conn_ = other.conn_;
            other.pool_ = nullptr;
        }
        return *this;
    }
    
    ~ConnectionGuard() {
        release();
    }
    
    TcpConnectionPtr connection() const { return conn_.connection; }
    bool valid() const { return conn_.connection && conn_.is_valid; }
    
private:
    void release() {
        if (pool_ && conn_.connection) {
            pool_->return_connection(conn_);
        }
        pool_ = nullptr;
        conn_.connection.reset();
    }
    
    ConnectionPool::Ptr pool_;
    PooledConnection conn_;
};

} // namespace net
} // namespace chwell