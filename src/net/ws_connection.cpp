#include "chwell/net/ws_connection.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace net {

WsConnection::WsConnection(asio::ip::tcp::socket socket)
    : socket_(std::move(socket)),
      read_buffer_(4096) {
}

void WsConnection::start() {
    // 当前仅作为占位实现：直接把收到的数据当作文本消息转发，
    // 尚未实现真正的 WebSocket 握手与帧解析。
    do_read();
}

void WsConnection::send_text(const std::string& text) {
    // 简化版：暂时直接发送原始文本数据（未加 WebSocket 帧头）
    // 后续可以在这里补齐 WebSocket 帧封装逻辑。
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(text),
        [this, self](const asio::error_code& ec, std::size_t bytes_transferred) {
            on_write(ec, bytes_transferred);
        });
}

void WsConnection::close() {
    asio::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

void WsConnection::do_read() {
    auto self = shared_from_this();
    socket_.async_read_some(asio::buffer(read_buffer_),
        [this, self](const asio::error_code& ec, std::size_t bytes_transferred) {
            on_read(ec, bytes_transferred);
        });
}

void WsConnection::on_read(const asio::error_code& ec, std::size_t bytes_transferred) {
    if (ec) {
        core::Logger::instance().warn("WsConnection closed: " + ec.message());
        if (close_cb_) {
            close_cb_(shared_from_this());
        }
        return;
    }

    // 占位实现：直接把收到的数据当作 UTF-8 文本传给回调
    std::string text(read_buffer_.begin(),
                     read_buffer_.begin() + static_cast<std::ptrdiff_t>(bytes_transferred));

    if (message_cb_) {
        message_cb_(shared_from_this(), text);
    }

    do_read();
}

void WsConnection::on_write(const asio::error_code& ec, std::size_t /*bytes_transferred*/) {
    if (ec) {
        core::Logger::instance().warn("WsConnection send failed: " + ec.message());
    }
}

} // namespace net
} // namespace chwell

