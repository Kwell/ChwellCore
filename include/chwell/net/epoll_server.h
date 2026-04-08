#pragma once

#include <memory>
#include <mutex>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <functional>

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
};

} // namespace net
} // namespace chwell
