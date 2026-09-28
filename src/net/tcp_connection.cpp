#include "chwell/net/tcp_connection.h"
#include "chwell/core/logger.h"
#include <cerrno>
#include <netinet/tcp.h>

namespace chwell {
namespace net {

TcpConnection::TcpConnection(TcpSocket socket)
    : socket_(std::move(socket)), read_buffer_(4096) {
    CHWELL_LOG_DEBUG("TcpConnection created");
}

TcpConnection::~TcpConnection() {
    // 读循环退出后 socket 已 close；未 start 的连接在此释放 fd
    std::lock_guard<std::mutex> lock(send_mutex_);
    ErrorCode ec;
    socket_.close(ec);
}

void TcpConnection::start() {
    // P0 #3: set a 30-second send timeout so send() cannot block forever
    // on a slow/malicious peer (prevents thread-pool exhaustion).
    struct timeval tv;
    tv.tv_sec  = 30;
    tv.tv_usec = 0;
    ::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
                 &tv, sizeof(tv));

    CHWELL_LOG_DEBUG("TcpConnection read loop starting");
    run_read_loop();
}

void TcpConnection::run_read_loop() {
    while (!closed_ && socket_.is_open()) {
        ssize_t n = socket_.read(read_buffer_.data(), read_buffer_.size());
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                CHWELL_LOG_WARN("Connection read error: " + std::string(strerror(errno)));
            }
            break;
        }

        if (message_cb_) {
            message_cb_(shared_from_this(),
                        std::string_view(read_buffer_.data(),
                                         static_cast<std::size_t>(n)));
        }
    }

    // P0 #1: call close_cb_ exactly once – no CloseGuard needed.
    closed_ = true;
    {
        // 与 send() 串行：避免 write(fd) 与 close(fd) 并发
        std::lock_guard<std::mutex> lock(send_mutex_);
        ErrorCode ec;
        socket_.shutdown(SHUT_RDWR, ec);
        socket_.close(ec);
    }
    closed_ = true;
    if (close_cb_) {
        close_cb_(shared_from_this());
    }
}

void TcpConnection::send(const std::vector<char>& data) {
    send(std::string_view(data.data(), data.size()));
}

void TcpConnection::send(std::string_view data) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (closed_ || !socket_.is_open()) {
        CHWELL_LOG_WARN("Send failed: connection closed");
        return;
    }
    CHWELL_LOG_DEBUG("Sending " << data.size() << " bytes");
    const char* ptr = data.data();
    std::size_t len = data.size();
    while (len > 0) {
        ssize_t n = socket_.write(ptr, len);
        if (n <= 0) {
            CHWELL_LOG_ERROR("Send failed, closing connection to avoid stream desync");
            net::ErrorCode ec;
            socket_.shutdown(SHUT_RDWR, ec);
            socket_.close(ec);
            closed_ = true;
            return;
        }
        ptr += n;
        len -= static_cast<std::size_t>(n);
    }
    CHWELL_LOG_DEBUG("Send completed");
}

void TcpConnection::close() {
    // P0 #2: double-check with send_mutex_ to avoid racing with send().
    if (closed_) {
        CHWELL_LOG_DEBUG("Connection already closed");
        return;
    }
    // 只 shutdown 唤醒读循环。真正 ::close(fd) 由 run_read_loop 收尾或析构完成，
    // 避免与阻塞中的 read(fd) 并发 close（tsan fd 资源竞争）以及 io 锁死锁。
    ErrorCode ec;
    socket_.shutdown(SHUT_RDWR, ec);
    if (ec) {
        CHWELL_LOG_WARN("Shutdown failed: " + ec.message());
    }
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (closed_) {
        CHWELL_LOG_DEBUG("Connection already closed (after lock)");
        return;
    }
    CHWELL_LOG_INFO("Closing connection");
    closed_ = true;
}

} // namespace net
} // namespace chwell
