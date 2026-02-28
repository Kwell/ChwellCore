#pragma once

#include <set>
#include <thread>
#include <atomic>

#include "chwell/net/posix_io.h"
#include "chwell/net/ws_connection.h"

namespace chwell {
namespace net {

class WsServer {
public:
    WsServer(IoService& io_service, unsigned short port);

    void start_accept();
    void stop();

    void set_message_callback(const WsMessageCallback& cb) { message_cb_ = cb; }
    void set_connection_callback(const WsConnectionCallback& cb) { connection_cb_ = cb; }
    void set_disconnect_callback(const WsConnectionCallback& cb) { disconnect_cb_ = cb; }

private:
    void accept_loop();

    IoService& io_service_;
    unsigned short port_;
    TcpAcceptor acceptor_;
    int wake_pipe_[2]{-1, -1};
    std::set<WsConnectionPtr> connections_;
    std::thread accept_thread_;
    std::atomic<bool> stopped_{false};
    WsMessageCallback message_cb_;
    WsConnectionCallback connection_cb_;
    WsConnectionCallback disconnect_cb_;
};

} // namespace net
} // namespace chwell
