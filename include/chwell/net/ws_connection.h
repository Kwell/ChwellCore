#pragma once

#include <memory>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

#include "chwell/net/posix_io.h"

namespace chwell {
namespace net {

class WsConnection;

typedef std::shared_ptr<WsConnection> WsConnectionPtr;
typedef std::function<void(const WsConnectionPtr&, const std::string&)> WsMessageCallback;
typedef std::function<void(const WsConnectionPtr&)> WsConnectionCallback;

// 非完整实现的 WebSocket 连接封装骨架
class WsConnection : public std::enable_shared_from_this<WsConnection> {
public:
    explicit WsConnection(TcpSocket socket);

    void start();
    void send_text(const std::string& text);
    void close();

    void set_message_callback(const WsMessageCallback& cb) { message_cb_ = cb; }
    void set_close_callback(const WsConnectionCallback& cb) { close_cb_ = cb; }

    int native_handle() const { return socket_.native_handle(); }

private:
    void run_read_loop();

    TcpSocket socket_;
    std::vector<char> read_buffer_;
    WsMessageCallback message_cb_;
    WsConnectionCallback close_cb_;
    std::atomic<bool> closed_{false};
    std::mutex send_mutex_;
};

} // namespace net
} // namespace chwell
