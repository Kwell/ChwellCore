#pragma once

#include <asio.hpp>
#include <memory>
#include <vector>
#include <functional>

namespace chwell {
namespace net {

class TcpConnection;

typedef std::shared_ptr<TcpConnection> TcpConnectionPtr;
typedef std::function<void(const TcpConnectionPtr&, const std::vector<char>&)> MessageCallback;
typedef std::function<void(const TcpConnectionPtr&)> ConnectionCallback;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    explicit TcpConnection(asio::ip::tcp::socket socket);

    void start();
    void send(const std::vector<char>& data);
    void close();

    void set_message_callback(const MessageCallback& cb) { message_cb_ = cb; }
    void set_close_callback(const ConnectionCallback& cb) { close_cb_ = cb; }

    asio::ip::tcp::socket& socket() { return socket_; }

private:
    void do_read();
    void on_read(const asio::error_code& ec, std::size_t bytes_transferred);
    void on_write(const asio::error_code& ec, std::size_t bytes_transferred);

    asio::ip::tcp::socket socket_;
    std::vector<char> read_buffer_;
    MessageCallback message_cb_;
    ConnectionCallback close_cb_;
};

} // namespace net
} // namespace chwell

