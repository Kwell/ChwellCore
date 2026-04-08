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
                    << " (reactors=" << reactors_.size() << ")");
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

} // namespace net
} // namespace chwell
