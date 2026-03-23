#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <chrono>

#include "chwell/net/udp_socket.h"
#include "chwell/core/logger.h"

using namespace chwell;

// ============================================
// UDP Socket 单元测试
// ============================================

TEST(UdpSocketTest, CreateUdpSocket) {
    auto socket = net::create_udp_socket("127.0.0.1", 0);
    EXPECT_NE(nullptr, socket);
    EXPECT_GE(socket->native_handle(), 0);
}

TEST(UdpSocketTest, BindPort) {
    auto socket = net::create_udp_socket("127.0.0.1", 9999);
    ASSERT_NE(nullptr, socket);

    bool bound = net::bind_udp_port(socket, "127.0.0.1", 9999);
    EXPECT_TRUE(bound);
    EXPECT_EQ(9999, socket->local_port());
}

TEST(UdpSocketTest, SendTo) {
    auto socket = net::create_udp_socket("127.0.0.1", 9998);
    ASSERT_NE(nullptr, socket);

    bool bound = net::bind_udp_port(socket, "127.0.0.1", 9998);
    ASSERT_TRUE(bound);

    // 测试发送
    std::string data = "Hello, UDP!";
    socket->send_to("127.0.0.1", 9999, data);

    // 等待一下让数据发送
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(UdpSocketTest, MultipleSockets) {
    auto socket1 = net::create_udp_socket("127.0.0.1", 9901);
    auto socket2 = net::create_udp_socket("127.0.0.1", 9902);

    ASSERT_NE(nullptr, socket1);
    ASSERT_NE(nullptr, socket2);

    EXPECT_TRUE(net::bind_udp_port(socket1, "127.0.0.1", 9901));
    EXPECT_TRUE(net::bind_udp_port(socket2, "127.0.0.1", 9902));

    EXPECT_EQ(9901, socket1->local_port());
    EXPECT_EQ(9902, socket2->local_port());

    socket2->send_to("127.0.0.1", 9901, "Hello from socket2");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(UdpSocketTest, SendReceive) {
    // 创建接收 socket
    auto recv_socket = net::create_udp_socket("127.0.0.1", 9900);
    ASSERT_NE(nullptr, recv_socket);
    ASSERT_TRUE(net::bind_udp_port(recv_socket, "127.0.0.1", 9900));

    // 创建发送 socket（也绑定临时端口）
    auto send_socket = net::create_udp_socket("127.0.0.1", 0);
    ASSERT_NE(nullptr, send_socket);
    ASSERT_TRUE(net::bind_udp_port(send_socket, "127.0.0.1", 0));

    // 发送消息到接收 socket 的端口
    std::string data = "Test message";
    send_socket->send_to("127.0.0.1", 9900, data);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
