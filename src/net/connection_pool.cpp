#include "chwell/net/connection_pool.h"
#include <cassert>
#include <thread>

namespace chwell {
namespace net {

ConnectionPool::ConnectionPool(IoService& io_service, const ConnectionPoolConfig& config)
    : io_service_(io_service)
    , config_(config)
    , shutdown_(false)
    , pending_creates_(0) {
}

ConnectionPool::~ConnectionPool() {
    shutdown();
}

bool ConnectionPool::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (shutdown_) {
        return false;
    }
    
    // 简化实现：不预创建连接，按需创建
    CHWELL_LOG_INFO("ConnectionPool initialized for " 
                  << config_.host << ":" << config_.port);
    
    return true;
}

void ConnectionPool::shutdown() {
    if (shutdown_.exchange(true)) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& conn : connections_) {
        if (conn && conn->connection) {
            conn->connection->close();
        }
    }
    connections_.clear();
    
    while (!waiting_callbacks_.empty()) {
        auto cb = waiting_callbacks_.front();
        waiting_callbacks_.pop();
        if (cb) {
            PooledConnection empty;
            cb(empty);
        }
    }
    
    cv_.notify_all();
    
    CHWELL_LOG_INFO("ConnectionPool shutdown: " << config_.host << ":" << config_.port);
}

bool ConnectionPool::create_connection(PooledConnection& conn) {
    // 简化实现：返回一个占位连接
    // 实际项目中需要实现真正的连接逻辑
    conn.connection.reset();  // 暂时返回空
    conn.create_time = PooledConnection::current_time_ms();
    conn.last_used_time = conn.create_time;
    conn.in_use = false;
    conn.is_valid = false;
    
    CHWELL_LOG_WARN("ConnectionPool::create_connection not fully implemented");
    return false;
}

bool ConnectionPool::validate_connection(PooledConnection& conn) {
    if (!conn.connection || !conn.is_valid) {
        return false;
    }
    
    int64_t now = PooledConnection::current_time_ms();
    
    if (config_.max_lifetime_ms > 0) {
        if (now - conn.create_time > config_.max_lifetime_ms) {
            conn.is_valid = false;
            return false;
        }
    }
    
    if (!conn.in_use) {
        if (now - conn.last_used_time > config_.idle_timeout_ms) {
            conn.is_valid = false;
            return false;
        }
    }
    
    return true;
}

void ConnectionPool::cleanup_expired() {
    auto it = connections_.begin();
    while (it != connections_.end()) {
        if (!validate_connection(**it) && !(*it)->in_use) {
            if ((*it)->connection) {
                (*it)->connection->close();
            }
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

void ConnectionPool::maybe_expand() {
    // 简化实现
}

bool ConnectionPool::try_get_idle(PooledConnection& conn) {
    for (auto& c : connections_) {
        if (!c->in_use && c->is_valid && validate_connection(*c)) {
            c->in_use = true;
            c->last_used_time = PooledConnection::current_time_ms();
            conn = *c;
            return true;
        }
    }
    return false;
}

void ConnectionPool::get_connection(ConnectionCallback callback, int timeout_ms) {
    if (shutdown_) {
        PooledConnection empty;
        callback(empty);
        return;
    }
    
    std::unique_lock<std::mutex> lock(mutex_);
    
    cleanup_expired();
    
    PooledConnection conn;
    if (try_get_idle(conn)) {
        lock.unlock();
        callback(conn);
        return;
    }
    
    // 暂时返回空连接
    lock.unlock();
    PooledConnection empty;
    callback(empty);
}

PooledConnection ConnectionPool::get_connection_sync(int timeout_ms) {
    PooledConnection result;
    std::mutex sync_mutex;
    std::condition_variable sync_cv;
    bool done = false;
    
    get_connection([&](const PooledConnection& conn) {
        std::lock_guard<std::mutex> lock(sync_mutex);
        result = conn;
        done = true;
        sync_cv.notify_one();
    }, timeout_ms);
    
    std::unique_lock<std::mutex> lock(sync_mutex);
    sync_cv.wait(lock, [&]() { return done; });
    
    return result;
}

void ConnectionPool::return_connection(PooledConnection& conn) {
    if (!conn.connection) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& c : connections_) {
        if (c->connection.get() == conn.connection.get()) {
            c->in_use = false;
            c->last_used_time = PooledConnection::current_time_ms();
            
            if (!c->is_valid) {
                c->connection->close();
            }
            break;
        }
    }
    
    conn.connection.reset();
    conn.is_valid = false;
    
    if (!waiting_callbacks_.empty()) {
        PooledConnection idle_conn;
        if (try_get_idle(idle_conn)) {
            auto cb = waiting_callbacks_.front();
            waiting_callbacks_.pop();
            if (cb) {
                cb(idle_conn);
            }
        }
    }
    
    cv_.notify_one();
}

int ConnectionPool::total_connections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(connections_.size());
}

int ConnectionPool::idle_connections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& conn : connections_) {
        if (!conn->in_use && conn->is_valid) {
            ++count;
        }
    }
    return count;
}

int ConnectionPool::active_connections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& conn : connections_) {
        if (conn->in_use) {
            ++count;
        }
    }
    return count;
}

} // namespace net
} // namespace chwell