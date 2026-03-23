#include "chwell/net/connection_pool.h"
#include <cassert>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>

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
    if (shutdown_) {
        return false;
    }

    // 预创建 min_connections 个连接（在锁外创建，避免阻塞锁）
    for (int i = 0; i < config_.min_connections; ++i) {
        auto pooled = std::make_unique<PooledConnection>();
        if (create_connection(*pooled)) {
            std::lock_guard<std::mutex> lock(mutex_);
            connections_.push_back(std::move(pooled));
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    CHWELL_LOG_INFO("ConnectionPool initialized for "
                  << config_.host << ":" << config_.port
                  << " (" << connections_.size() << "/" << config_.min_connections
                  << " pre-created)");
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
    if (config_.host.empty() || config_.port == 0) {
        CHWELL_LOG_ERROR("ConnectionPool: invalid host/port (host="
                       << config_.host << " port=" << config_.port << ")");
        return false;
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) <= 0) {
        CHWELL_LOG_ERROR("ConnectionPool: invalid host address: " + config_.host);
        return false;
    }

    // 创建非阻塞 socket，以便支持连接超时
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        CHWELL_LOG_ERROR("ConnectionPool: socket() failed: " + std::string(strerror(errno)));
        return false;
    }

    int ret = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        CHWELL_LOG_ERROR("ConnectionPool: connect() failed: " + std::string(strerror(errno)));
        ::close(fd);
        return false;
    }

    if (ret < 0) {
        // EINPROGRESS：用 poll 等待连接完成
        int timeout_ms = config_.connect_timeout_ms > 0 ? config_.connect_timeout_ms : 5000;
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int r = poll(&pfd, 1, timeout_ms);
        if (r <= 0) {
            CHWELL_LOG_ERROR("ConnectionPool: connect timeout to "
                           + config_.host + ":" + std::to_string(config_.port));
            ::close(fd);
            return false;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            CHWELL_LOG_ERROR("ConnectionPool: connect error: " + std::string(strerror(err)));
            ::close(fd);
            return false;
        }
    }

    // 切回阻塞模式（与 TcpSocket 阻塞 read/write 一致）
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    conn.connection = std::make_shared<TcpConnection>(TcpSocket(fd));
    conn.create_time = PooledConnection::current_time_ms();
    conn.last_used_time = conn.create_time;
    conn.in_use = false;
    conn.is_valid = true;

    CHWELL_LOG_INFO("ConnectionPool: new connection to "
                  + config_.host + ":" + std::to_string(config_.port));
    return true;
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
    // 若当前空闲连接不足 min_connections，尝试补充（在持有 mutex_ 的情况下调用时需注意）
    int total = static_cast<int>(connections_.size()) + pending_creates_.load();
    if (total >= config_.min_connections || total >= config_.max_connections) {
        return;
    }
    ++pending_creates_;
    // 启动线程异步补充，避免在锁内阻塞
    std::thread([this]() {
        PooledConnection conn;
        if (create_connection(conn)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!shutdown_) {
                connections_.push_back(std::make_unique<PooledConnection>(conn));
            }
        }
        --pending_creates_;
    }).detach();
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

    {
        std::unique_lock<std::mutex> lock(mutex_);

        cleanup_expired();

        PooledConnection conn;
        if (try_get_idle(conn)) {
            lock.unlock();
            callback(conn);
            return;
        }

        // 未超过最大连接数时，先记录 pending 然后锁外创建
        int total = static_cast<int>(connections_.size()) + pending_creates_.load();
        if (total < config_.max_connections) {
            ++pending_creates_;
        } else {
            // 已达上限：排队等待（timeout_ms==0 则立即返回空）
            if (timeout_ms != 0) {
                waiting_callbacks_.push(callback);
            } else {
                lock.unlock();
                PooledConnection empty;
                callback(empty);
            }
            return;
        }
    }

    // 在锁外实际建立 TCP 连接，避免阻塞池锁
    PooledConnection conn;
    bool ok = create_connection(conn);
    --pending_creates_;

    if (!ok) {
        PooledConnection empty;
        callback(empty);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!shutdown_) {
            conn.in_use = true;
            conn.last_used_time = PooledConnection::current_time_ms();
            connections_.push_back(std::make_unique<PooledConnection>(conn));
        }
    }

    callback(conn);
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