#pragma once

#include <asio.hpp>
#include <set>
#include "chwell/net/ws_connection.h"

namespace chwell {
namespace net {

// WebSocket 服务器骨架：
// - 目前未实现完整握手，仅作为未来扩展的占位实现。
class WsServer {
public:
    WsServer(asio::io_service& io_service, unsigned short port);

    void start_accept();

    void set_message_callback(const WsMessageCallback& cb) { message_cb_ = cb; }
    void set_connection_callback(const WsConnectionCallback& cb) { connection_cb_ = cb; }
    void set_disconnect_callback(const WsConnectionCallback& cb) { disconnect_cb_ = cb; }

private:
    void do_accept();
    void on_accept(const asio::error_code& ec, asio::ip::tcp::socket socket);

    asio::io_service& io_service_;
    asio::ip::tcp::acceptor acceptor_;
    std::set<WsConnectionPtr> connections_;
    WsMessageCallback message_cb_;
    WsConnectionCallback connection_cb_;
    WsConnectionCallback disconnect_cb_;
};

} // namespace net
} // namespace chwell

