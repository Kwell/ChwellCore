#include "chwell/net/tcp_connection.h"
#include "chwell/core/logger.h"
#include <cerrno>

namespace chwell {
namespace net {

TcpConnection::TcpConnection(TcpSocket socket)
    : socket_(std::move(socket)), read_buffer_(4096) {
    CHWELL_LOG_DEBUG("TcpConnection created");
}

void TcpConnection::start() {
    CHWELL_LOG_DEBUG("TcpConnection read loop starting");
    run_read_loop();
}

void TcpConnection::run_read_loop() {
    struct CloseGuard {
        TcpConnection& conn;
        bool active{true};

        explicit CloseGuard(TcpConnection& c) noexcept
            : conn(c) {}

        ~CloseGuard() noexcept {
            if (!active) {
                return;
            }
            conn.closed_ = true;
            if (conn.close_cb_) {
                conn.close_cb_(conn.shared_from_this());
            }
        }
    } guard(*this);

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

    guard.active = false;
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
            CHWELL_LOG_ERROR("Send failed: " + std::string(strerror(errno)));
            return;
        }
        ptr += n;
        len -= static_cast<std::size_t>(n);
    }
    CHWELL_LOG_DEBUG("Send completed");
}

void TcpConnection::close() {
    if (closed_) {
        CHWELL_LOG_DEBUG("Connection already closed");
        return;
    }
    CHWELL_LOG_INFO("Closing connection");
    closed_ = true;
    ErrorCode ec;
    socket_.shutdown(SHUT_RDWR, ec);
    if (ec) {
        CHWELL_LOG_WARN("Shutdown failed: " + ec.message());
    }
    socket_.close(ec);
    if (ec) {
        CHWELL_LOG_WARN("Close failed: " + ec.message());
    }
}

} // namespace net
} // namespace chwell
