#include "chwell/net/ws_connection.h"
#include "chwell/core/logger.h"
#include <cerrno>
#include <sys/socket.h>

namespace chwell {
namespace net {

WsRawConnection::WsRawConnection(TcpSocket socket)
    : socket_(std::move(socket)), read_buffer_(4096) {
    // 设置发送超时，防止慢客户端导致线程永久阻塞
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    ::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void WsRawConnection::start() {
    run_read_loop();
}

void WsRawConnection::run_read_loop() {
    // Capture self to keep the object alive for the duration of this loop,
    // preventing shared_from_this() from throwing in the close callback
    // when the last external shared_ptr has already been released.
    auto self = shared_from_this();

    while (!closed_ && socket_.is_open()) {
        ssize_t n = socket_.read(read_buffer_.data(), read_buffer_.size());
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                CHWELL_LOG_WARN("WsRawConnection read error: " + std::string(strerror(errno)));
            }
            break;
        }

        std::string text(read_buffer_.begin(), read_buffer_.begin() + n);
        if (message_cb_) {
            message_cb_(self, text);
        }
    }

    closed_ = true;
    if (close_cb_) {
        close_cb_(self);
    }
}

void WsRawConnection::send_text(const std::string& text) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (closed_ || !socket_.is_open()) return;
    const char* ptr = text.data();
    std::size_t len = text.size();
    while (len > 0) {
        ssize_t n = socket_.write(ptr, len);
        if (n <= 0) {
            CHWELL_LOG_WARN("WsRawConnection send failed: " + std::string(strerror(errno)));
            // 半包写失败必须断开，否则后续 send 会接在半帧后造成流错乱
            ErrorCode ec;
            socket_.shutdown(SHUT_RDWR, ec);
            socket_.close(ec);
            closed_ = true;
            return;
        }
        ptr += n;
        len -= static_cast<std::size_t>(n);
    }
}

void WsRawConnection::send_binary(const std::vector<char>& data) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (closed_ || !socket_.is_open()) return;
    const char* ptr = data.data();
    std::size_t len = data.size();
    while (len > 0) {
        ssize_t n = socket_.write(ptr, len);
        if (n <= 0) {
            CHWELL_LOG_WARN("WsRawConnection send_binary failed: " + std::string(strerror(errno)));
            ErrorCode ec;
            socket_.shutdown(SHUT_RDWR, ec);
            socket_.close(ec);
            closed_ = true;
            return;
        }
        ptr += n;
        len -= static_cast<std::size_t>(n);
    }
}

void WsRawConnection::close() {
    closed_ = true;
    // Acquire send_mutex_ to synchronize with any in-progress send_text/send_binary
    // calls, preventing a race between send() and shutdown()/close().
    std::lock_guard<std::mutex> lock(send_mutex_);
    ErrorCode ec;
    socket_.shutdown(SHUT_RDWR, ec);
    socket_.close(ec);
}

} // namespace net
} // namespace chwell
