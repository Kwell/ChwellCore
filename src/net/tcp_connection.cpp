#include "chwell/net/tcp_connection.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace net {

TcpConnection::TcpConnection(asio::ip::tcp::socket socket)
    : socket_(std::move(socket)), read_buffer_(4096) {
}

void TcpConnection::start() {
    do_read();
}

void TcpConnection::send(const std::vector<char>& data) {
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(data),
        [this, self](const asio::error_code& ec, std::size_t bytes_transferred) {
            on_write(ec, bytes_transferred);
        });
}

void TcpConnection::close() {
    asio::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

void TcpConnection::do_read() {
    auto self = shared_from_this();
    socket_.async_read_some(asio::buffer(read_buffer_),
        [this, self](const asio::error_code& ec, std::size_t bytes_transferred) {
            on_read(ec, bytes_transferred);
        });
}

void TcpConnection::on_read(const asio::error_code& ec, std::size_t bytes_transferred) {
    if (ec) {
        core::Logger::instance().warn("Connection closed: " + ec.message());
        if (close_cb_) {
            close_cb_(shared_from_this());
        }
        return;
    }

    std::vector<char> msg(read_buffer_.begin(), read_buffer_.begin() + static_cast<std::ptrdiff_t>(bytes_transferred));
    if (message_cb_) {
        message_cb_(shared_from_this(), msg);
    }

    do_read();
}

void TcpConnection::on_write(const asio::error_code& ec, std::size_t /*bytes_transferred*/) {
    if (ec) {
        core::Logger::instance().warn("Send failed: " + ec.message());
    }
}

} // namespace net
} // namespace chwell

