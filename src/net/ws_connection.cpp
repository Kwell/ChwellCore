#include "chwell/net/ws_connection.h"
#include "chwell/core/logger.h"
#include <cerrno>

namespace chwell {
namespace net {

WsConnection::WsConnection(TcpSocket socket)
    : socket_(std::move(socket)), read_buffer_(4096) {
}

void WsConnection::start() {
    run_read_loop();
}

void WsConnection::run_read_loop() {
    while (!closed_ && socket_.is_open()) {
        ssize_t n = socket_.read(read_buffer_.data(), read_buffer_.size());
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                core::Logger::instance().warn("WsConnection read error: " + std::string(strerror(errno)));
            }
            break;
        }

        std::string text(read_buffer_.begin(), read_buffer_.begin() + n);
        if (message_cb_) {
            message_cb_(shared_from_this(), text);
        }
    }

    closed_ = true;
    if (close_cb_) {
        close_cb_(shared_from_this());
    }
}

void WsConnection::send_text(const std::string& text) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (closed_ || !socket_.is_open()) return;
    const char* ptr = text.data();
    std::size_t len = text.size();
    while (len > 0) {
        ssize_t n = socket_.write(ptr, len);
        if (n <= 0) {
            core::Logger::instance().warn("WsConnection send failed: " + std::string(strerror(errno)));
            return;
        }
        ptr += n;
        len -= static_cast<std::size_t>(n);
    }
}

void WsConnection::close() {
    closed_ = true;
    ErrorCode ec;
    socket_.shutdown(SHUT_RDWR, ec);
    socket_.close(ec);
}

} // namespace net
} // namespace chwell
