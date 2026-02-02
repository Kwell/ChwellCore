#include "chwell/net/tcp_server.h"
#include "chwell/core/logger.h"
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace chwell {
namespace net {

TcpServer::TcpServer(IoService& io_service, unsigned short port)
    : io_service_(io_service), acceptor_(port) {
}

void TcpServer::start_accept() {
    if (acceptor_.listen_fd() < 0) {
        core::Logger::instance().error("TcpServer: failed to create acceptor");
        return;
    }

    if (pipe(wake_pipe_) != 0) {
        core::Logger::instance().error("TcpServer: failed to create wake pipe");
        return;
    }

    stopped_ = false;
    accept_thread_ = std::thread([this]() { accept_loop(); });
}

void TcpServer::stop() {
    stopped_ = true;
    if (wake_pipe_[1] >= 0) {
        char c = 1;
        ssize_t n = write(wake_pipe_[1], &c, 1);
        (void)n;  // best-effort wake during shutdown
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (wake_pipe_[0] >= 0) { close(wake_pipe_[0]); wake_pipe_[0] = -1; }
    if (wake_pipe_[1] >= 0) { close(wake_pipe_[1]); wake_pipe_[1] = -1; }
}

void TcpServer::accept_loop() {
    pollfd fds[2];
    fds[0].fd = acceptor_.listen_fd();
    fds[0].events = POLLIN;
    fds[1].fd = wake_pipe_[0];
    fds[1].events = POLLIN;

    while (!stopped_) {
        int ret = poll(fds, 2, 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            core::Logger::instance().error("TcpServer poll error: " + std::string(strerror(errno)));
            break;
        }
        if (ret == 0) continue;

        if (fds[1].revents & POLLIN) {
            char buf[64];
            ssize_t n = read(wake_pipe_[0], buf, sizeof(buf));
            (void)n;  // drain wake pipe
            break;
        }

        if (fds[0].revents & POLLIN) {
            ErrorCode ec;
            TcpSocket socket = acceptor_.accept(ec);
            if (ec) {
                core::Logger::instance().error("Accept failed: " + ec.message());
                continue;
            }

            auto conn = std::make_shared<TcpConnection>(std::move(socket));
            conn->set_message_callback(message_cb_);
            conn->set_close_callback([this](const TcpConnectionPtr& c) {
                connections_.erase(c);
                if (disconnect_cb_) {
                    disconnect_cb_(c);
                }
            });

            connections_.insert(conn);

            if (connection_cb_) {
                connection_cb_(conn);
            }

            io_service_.post([conn, this]() {
                conn->start();
            });
        }
    }
}

} // namespace net
} // namespace chwell
