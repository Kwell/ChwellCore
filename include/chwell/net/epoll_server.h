#pragma once

#include <memory>
#include <mutex>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <chrono>

#include "chwell/net/epoll_demuxer.h"
#include "chwell/net/epoll_connection.h"

namespace chwell {
namespace net {

class EpollTcpServer {
public:
    using MessageCallback = EpollMessageCallback;
    using ConnectionCallback = EpollConnectionCallback;

    explicit EpollTcpServer(unsigned short port, int reactor_threads = 1);
    ~EpollTcpServer();

    void start();
    void stop();

    bool is_valid() const { return listen_fd_ >= 0; }
    unsigned short port() const { return port_; }

    void set_message_callback(const MessageCallback& cb) { message_cb_ = cb; }
    void set_connection_callback(const ConnectionCallback& cb) { connection_cb_ = cb; }
    void set_disconnect_callback(const ConnectionCallback& cb) { disconnect_cb_ = cb; }

    size_t connection_count() const {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        return connections_.size();
    }

    // ========== 安全限制（P0-2） ==========

    // 设置最大连接数，默认 10000
    void set_max_connections(size_t max_conn) { max_connections_ = max_conn; }
    size_t max_connections() const { return max_connections_; }

    // 设置空闲连接超时检查间隔（秒），0 表示不检查，默认 10 秒
    void set_idle_check_interval(int seconds) { idle_check_interval_sec_ = seconds; }

    // 手动清理空闲连接（也可由定时器周期调用）
    void cleanup_idle_connections();

    // 在新连接到达时顺便检查空闲连接
    void maybe_cleanup_idle_connections();

private:
    void on_new_connection(int client_fd);
    void on_connection_close(const EpollTcpConnectionPtr& conn);
    void remove_connection(const EpollTcpConnectionPtr& conn);

    unsigned short port_;
    int listen_fd_{-1};
    std::atomic<bool> stopped_{false};

    struct ReactorThread {
        std::thread thread;
        std::unique_ptr<EpollDemuxer> demuxer;
    };
    std::vector<ReactorThread> reactors_;
    std::atomic<size_t> next_reactor_{0};

    std::thread accept_thread_;
    std::unique_ptr<EpollDemuxer> accept_demuxer_;

    mutable std::mutex conn_mutex_;
    std::unordered_set<EpollTcpConnectionPtr> connections_;

    MessageCallback message_cb_;
    ConnectionCallback connection_cb_;
    ConnectionCallback disconnect_cb_;

    // ========== 安全限制 ==========
    size_t max_connections_ = 10000;
    int idle_check_interval_sec_ = 10;
    std::chrono::steady_clock::time_point last_idle_check_time_;
};

} // namespace net
} // namespace chwell
