#pragma once

#include <asio.hpp>
#include <functional>
#include <vector>

namespace chwell {
namespace net {

class UdpServer;

typedef std::function<void(const std::vector<char>& data,
                           const asio::ip::udp::endpoint& remote)> UdpMessageCallback;

// 简单的 UDP 服务器封装：单端口收发
class UdpServer {
public:
    UdpServer(asio::io_service& io_service, unsigned short port);

    // 启动异步接收
    void start_receive();

    // 发送数据到指定地址
    void send_to(const std::vector<char>& data,
                 const asio::ip::udp::endpoint& remote);

    void set_message_callback(const UdpMessageCallback& cb) { message_cb_ = cb; }

private:
    void do_receive();
    void on_receive(const asio::error_code& ec, std::size_t bytes_transferred);

    asio::io_service& io_service_;
    asio::ip::udp::socket socket_;
    std::vector<char> buffer_;
    asio::ip::udp::endpoint remote_endpoint_;
    UdpMessageCallback message_cb_;
};

} // namespace net
} // namespace chwell

