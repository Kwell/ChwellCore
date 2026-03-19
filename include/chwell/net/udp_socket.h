#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

#include "chwell/net/socket_base.h"

namespace chwell {
namespace net {

// UDP 连接（无状态）
class UdpSocket {
public:
    using MessageCallback = std::function<void(const UdpSocketPtr&, const std::string&, const std::string&, uint16_t)>;
    using CloseCallback = std::function<void(const UdpSocketPtr&)>;

    UdpSocket(int fd);
    ~UdpSocket();

    // 启动（UDP 不需要启动，直接可以用）
    void start() {}

    // 发送数据（到指定地址）
    void send_to(const std::string& host, uint16_t port, const std::string& data);

    // 关闭
    void close();

    // 设置回调
    void set_message_callback(const MessageCallback& cb) { msg_cb_ = cb; }
    void set_close_callback(const CloseCallback& cb) { close_cb_ = cb; }

    // 获取文件描述符
    int native_handle() const noexcept { return fd_; }

    // 获取本地地址
    std::string local_addr() const { return local_addr_; }
    uint16_t local_port() const { return local_port_; }

private:
    int fd_;
    std::string local_addr_;
    uint16_t local_port_;
    MessageCallback msg_cb_;
    CloseCallback close_cb_;
};

// 创建 UDP socket
UdpSocketPtr create_udp_socket(const std::string& host, uint16_t port);

// 绑定 UDP 端口
bool bind_udp_port(UdpSocketPtr& socket, const std::string& host, uint16_t port);

} // namespace net
} // namespace chwell
