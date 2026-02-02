#pragma once

#include <memory>
#include <set>
#include <thread>
#include <atomic>

#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_connection.h"

namespace chwell {
namespace net {

class TcpServer {
public:
    TcpServer(IoService& io_service, unsigned short port);

    void start_accept();
    void stop();

    void set_message_callback(const MessageCallback& cb) { message_cb_ = cb; }
    void set_connection_callback(const ConnectionCallback& cb) { connection_cb_ = cb; }
    void set_disconnect_callback(const ConnectionCallback& cb) { disconnect_cb_ = cb; }

private:
    void accept_loop();

    IoService& io_service_;
    TcpAcceptor acceptor_;
    int wake_pipe_[2]{-1, -1};
    std::set<TcpConnectionPtr> connections_;
    std::thread accept_thread_;
    std::atomic<bool> stopped_{false};
    MessageCallback message_cb_;
    ConnectionCallback connection_cb_;
    ConnectionCallback disconnect_cb_;
};

} // namespace net
} // namespace chwell
