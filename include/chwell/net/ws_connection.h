#pragma once

#include <asio.hpp>
#include <memory>
#include <vector>
#include <functional>

namespace chwell {
namespace net {

class WsConnection;

typedef std::shared_ptr<WsConnection> WsConnectionPtr;
typedef std::function<void(const WsConnectionPtr&, const std::string&)> WsMessageCallback;
typedef std::function<void(const WsConnectionPtr&)> WsConnectionCallback;

// 非完整实现的 WebSocket 连接封装骨架：
// - 目标是提供统一接口，后续可以逐步补全握手和帧解析逻辑
class WsConnection : public std::enable_shared_from_this<WsConnection> {
public:
    explicit WsConnection(asio::ip::tcp::socket socket);

    void start();
    void send_text(const std::string& text);
    void close();

    void set_message_callback(const WsMessageCallback& cb) { message_cb_ = cb; }
    void set_close_callback(const WsConnectionCallback& cb) { close_cb_ = cb; }

    asio::ip::tcp::socket& socket() { return socket_; }

private:
    void do_read();
    void on_read(const asio::error_code& ec, std::size_t bytes_transferred);
    void on_write(const asio::error_code& ec, std::size_t bytes_transferred);

    // TODO: 后续可以在这里补充：握手状态、帧缓冲、掩码处理等

    asio::ip::tcp::socket socket_;
    std::vector<char> read_buffer_;
    WsMessageCallback message_cb_;
    WsConnectionCallback close_cb_;
};

} // namespace net
} // namespace chwell

