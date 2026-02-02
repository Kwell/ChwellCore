#include "chwell/net/tcp_server.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace net {

TcpServer::TcpServer(asio::io_service& io_service, unsigned short port)
    : io_service_(io_service),
      acceptor_(io_service, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {
}

void TcpServer::start_accept() {
    do_accept();
}

void TcpServer::do_accept() {
    acceptor_.async_accept(
        [this](const asio::error_code& ec, asio::ip::tcp::socket socket) {
            on_accept(ec, std::move(socket));
        });
}

void TcpServer::on_accept(const asio::error_code& ec, asio::ip::tcp::socket socket) {
    if (ec) {
        core::Logger::instance().error("Accept failed: " + ec.message());
        do_accept();
        return;
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

    conn->start();
    do_accept();
}

} // namespace net
} // namespace chwell

