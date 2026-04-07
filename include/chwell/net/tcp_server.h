#pragma once

#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <atomic>
#include <vector>

#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_connection.h"

namespace chwell {
namespace net {

class TcpServer {
public:
    /**
     * @brief Construct a TCP server listening on the given port.
     *
     * @note ARCHITECTURE LIMITATION (P1 #10): Each connection permanently occupies
     * a thread pool thread because TcpConnection::start() runs a blocking read
     * loop. This means max concurrent connections == IoService thread pool size.
     * For high-concurrency scenarios (hundreds+ connections), a future refactor
     * to epoll/non-blocking I/O is required.
     */
    TcpServer(IoService& io_service, unsigned short port);

    void start_accept();
    void stop();

    /// Returns true if the acceptor's listen socket is valid and ready to accept.
    bool is_valid() const { return acceptor_.listen_fd() >= 0; }

    void set_message_callback(const MessageCallback& cb) { message_cb_ = cb; }
    void set_connection_callback(const ConnectionCallback& cb) { connection_cb_ = cb; }
    void set_disconnect_callback(const ConnectionCallback& cb) { disconnect_cb_ = cb; }

private:
    void accept_loop();

    IoService& io_service_;
    unsigned short port_;
    TcpAcceptor acceptor_;
    int wake_pipe_[2]{-1, -1};
    std::mutex connections_mutex_;
    std::set<TcpConnectionPtr> connections_;
    std::thread accept_thread_;
    std::atomic<bool> stopped_{false};
    MessageCallback message_cb_;
    ConnectionCallback connection_cb_;
    ConnectionCallback disconnect_cb_;
};

} // namespace net
} // namespace chwell
