#include "chwell/net/epoll_server.h"
#include "chwell/core/logger.h"
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>

namespace chwell {
namespace net {

EpollTcpServer::EpollTcpServer(unsigned short port, int reactor_threads)
    : port_(port) {

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        CHWELL_LOG_ERROR("EpollTcpServer: socket() failed: " + std::string(std::strerror(errno)));
        return;
    }

    set_nonblocking(listen_fd_);

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        CHWELL_LOG_ERROR("EpollTcpServer: bind() failed: " + std::string(std::strerror(errno)));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    if (::listen(listen_fd_, 1024) < 0) {
        CHWELL_LOG_ERROR("EpollTcpServer: listen() failed: " + std::string(std::strerror(errno)));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    accept_demuxer_ = std::make_unique<EpollDemuxer>(64);

    for (int i = 0; i < reactor_threads; ++i) {
        ReactorThread rt;
        rt.demuxer = std::make_unique<EpollDemuxer>(1024);
        reactors_.push_back(std::move(rt));
    }

    last_idle_check_time_ = std::chrono::steady_clock::now();
}

EpollTcpServer::~EpollTcpServer() {
    stop();
}

void EpollTcpServer::start() {
    if (listen_fd_ < 0 || !accept_demuxer_) {
        CHWELL_LOG_ERROR("EpollTcpServer::start: invalid state");
        return;
    }

    accept_demuxer_->add(listen_fd_, IoEvent::Read,
        [this](int fd, IoEvent events) {
            if (has_event(events, IoEvent::Error)) {
                CHWELL_LOG_ERROR("EpollTcpServer: listen fd error");
                return;
            }
            while (!stopped_) {
                struct sockaddr_in client_addr{};
                socklen_t addr_len = sizeof(client_addr);
                int client_fd = ::accept4(fd, reinterpret_cast<struct sockaddr*>(&client_addr),
                                          &addr_len, SOCK_NONBLOCK);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    CHWELL_LOG_ERROR("EpollTcpServer: accept failed: " + std::string(std::strerror(errno)));
                    break;
                }
                on_new_connection(client_fd);
            }
        });

    stopped_ = false;

    for (auto& rt : reactors_) {
        rt.thread = std::thread([demuxer = rt.demuxer.get()]() {
            demuxer->run();
        });
    }

    accept_thread_ = std::thread([this]() {
        accept_demuxer_->run();
    });

    CHWELL_LOG_INFO("EpollTcpServer listening on 0.0.0.0:" << port_
                    << " (reactors=" << reactors_.size()
                    << ", max_connections=" << max_connections_ << ")");
}

void EpollTcpServer::stop() {
    if (stopped_.exchange(true)) return;
    CHWELL_LOG_INFO("EpollTcpServer stopping on port " << port_);

    if (accept_demuxer_) accept_demuxer_->stop();
    if (accept_thread_.joinable()) accept_thread_.join();

    for (auto& rt : reactors_) {
        if (rt.demuxer) rt.demuxer->stop();
    }
    for (auto& rt : reactors_) {
        if (rt.thread.joinable()) rt.thread.join();
    }

    std::vector<EpollTcpConnectionPtr> conns;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        conns.assign(connections_.begin(), connections_.end());
        connections_.clear();
    }
    for (auto& conn : conns) conn->close();

    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    CHWELL_LOG_INFO("EpollTcpServer stopped");
}

void EpollTcpServer::on_new_connection(int client_fd) {
    // 🆕 连接数上限检查（P0-2 半包攻击防护）
    size_t current_count = connection_count();
    if (current_count >= max_connections_) {
        CHWELL_LOG_WARN("Max connections reached (" << max_connections_
                        << "), rejecting fd=" << client_fd);
        ::close(client_fd);
        return;
    }

    size_t idx = next_reactor_.fetch_add(1) % reactors_.size();
    EpollDemuxer* demuxer = reactors_[idx].demuxer.get();

    auto conn = std::make_shared<EpollTcpConnection>(client_fd, demuxer);

    conn->set_message_callback([this](const EpollTcpConnectionPtr& c, std::string_view data) {
        if (message_cb_) message_cb_(c, data);
    });

    conn->set_close_callback([this](const EpollTcpConnectionPtr& c) {
        on_connection_close(c);
    });

    conn->bind_demuxer(demuxer);
    conn->start();

    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        connections_.insert(conn);
    }

    CHWELL_LOG_INFO("EpollTcpServer: new connection fd=" << client_fd
                    << ", total=" << connection_count());

    if (connection_cb_) connection_cb_(conn);

    // 🆕 空闲连接检查
    maybe_cleanup_idle_connections();
}

void EpollTcpServer::on_connection_close(const EpollTcpConnectionPtr& conn) {
    remove_connection(conn);
    if (disconnect_cb_) disconnect_cb_(conn);
}

void EpollTcpServer::remove_connection(const EpollTcpConnectionPtr& conn) {
    size_t remaining = 0;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        connections_.erase(conn);
        remaining = connections_.size();
    }
    CHWELL_LOG_INFO("EpollTcpServer: connection closed, remaining=" << remaining);
}

void EpollTcpServer::cleanup_idle_connections() {
    auto now = std::chrono::steady_clock::now();
    std::vector<EpollTcpConnectionPtr> to_close;

    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        for (auto& conn : connections_) {
            if (conn->is_idle_timeout()) {
                to_close.push_back(conn);
            }
        }
    }

    for (auto& conn : to_close) {
        CHWELL_LOG_WARN("Closing idle connection fd=" << conn->native_handle()
                        << ", idle_timeout=" << conn->idle_timeout_sec() << "s");
        conn->close();
    }

    if (!to_close.empty()) {
        CHWELL_LOG_INFO("Cleaned up " << to_close.size() << " idle connections");
    }
}

// 空闲检查：在新连接到达时顺便检查，避免额外的定时器线程
void EpollTcpServer::maybe_cleanup_idle_connections() {
    if (idle_check_interval_sec_ <= 0) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_idle_check_time_).count();

    if (elapsed >= static_cast<int64_t>(idle_check_interval_sec_)) {
        last_idle_check_time_ = now;
        cleanup_idle_connections();
    }
}

} // namespace net
} // namespace chwell
