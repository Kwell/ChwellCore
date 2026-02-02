#pragma once

#include <asio.hpp>
#include <memory>
#include <set>
#include "chwell/net/tcp_connection.h"

namespace chwell {
namespace net {

class TcpServer {
public:
    TcpServer(asio::io_service& io_service, unsigned short port);

    void start_accept();

    void set_message_callback(const MessageCallback& cb) { message_cb_ = cb; }
    void set_connection_callback(const ConnectionCallback& cb) { connection_cb_ = cb; }
    void set_disconnect_callback(const ConnectionCallback& cb) { disconnect_cb_ = cb; }

private:
    void do_accept();
    void on_accept(const asio::error_code& ec, asio::ip::tcp::socket socket);

    asio::io_service& io_service_;
    asio::ip::tcp::acceptor acceptor_;
    std::set<TcpConnectionPtr> connections_;
    MessageCallback message_cb_;
    ConnectionCallback connection_cb_;
    ConnectionCallback disconnect_cb_;
};

} // namespace net
} // namespace chwell

