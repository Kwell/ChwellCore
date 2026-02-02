#pragma once

#include <memory>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

#include "chwell/net/posix_io.h"

namespace chwell {
namespace net {

class TcpConnection;

typedef std::shared_ptr<TcpConnection> TcpConnectionPtr;
typedef std::function<void(const TcpConnectionPtr&, const std::vector<char>&)> MessageCallback;
typedef std::function<void(const TcpConnectionPtr&)> ConnectionCallback;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    explicit TcpConnection(TcpSocket socket);

    void start();
    void send(const std::vector<char>& data);
    void close();

    void set_message_callback(const MessageCallback& cb) { message_cb_ = cb; }
    void set_close_callback(const ConnectionCallback& cb) { close_cb_ = cb; }

    int native_handle() const { return socket_.native_handle(); }

private:
    void run_read_loop();

    TcpSocket socket_;
    std::vector<char> read_buffer_;
    MessageCallback message_cb_;
    ConnectionCallback close_cb_;
    std::atomic<bool> closed_{false};
    std::mutex send_mutex_;
};

} // namespace net
} // namespace chwell
