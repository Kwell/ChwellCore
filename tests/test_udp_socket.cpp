#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

#include "chwell/core/logger.h"
#include "chwell/net/udp_socket.h"

using namespace chwell;

// ============================================
// UDP Socket 单元测试
// ============================================

// 辅助：通过 poll+recvfrom 从非阻塞 socket 读取一条 UDP 数据报，
// 最多等待 timeout_ms 毫秒。返回收到的内容，超时则返回空串。
static std::string udp_recv_once(int fd, int timeout_ms = 500) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, timeout_ms) <= 0) return {};
    char buf[4096];
    ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0, nullptr, nullptr);
    if (n <= 0) return {};
    return std::string(buf, static_cast<std::size_t>(n));
}

TEST(UdpSocketTest, CreateUdpSocket) {
    // port=0: 仅创建，不绑定
    auto socket = net::create_udp_socket("127.0.0.1", 0);
    EXPECT_NE(nullptr, socket);
    EXPECT_GE(socket->native_handle(), 0);
}

TEST(UdpSocketTest, BindPort) {
    // create_udp_socket 传 port=0 则不绑定，再单独调用 bind_udp_port
    auto socket = net::create_udp_socket("127.0.0.1", 0);
    ASSERT_NE(nullptr, socket);

    bool bound = net::bind_udp_port(socket, "127.0.0.1", 19999);
    EXPECT_TRUE(bound);
    EXPECT_EQ(19999, socket->local_port());
}

TEST(UdpSocketTest, CreateWithPortBindsAutomatically) {
    // create_udp_socket 传非零端口时应自动完成绑定
    auto socket = net::create_udp_socket("127.0.0.1", 19998);
    ASSERT_NE(nullptr, socket);
    EXPECT_EQ(19998, socket->local_port());
    EXPECT_EQ("127.0.0.1", socket->local_addr());
}

TEST(UdpSocketTest, MultipleSockets) {
    // 两个 socket 各自绑定不同端口
    auto socket1 = net::create_udp_socket("127.0.0.1", 19901);
    auto socket2 = net::create_udp_socket("127.0.0.1", 19902);

    ASSERT_NE(nullptr, socket1);
    ASSERT_NE(nullptr, socket2);

    EXPECT_EQ(19901, socket1->local_port());
    EXPECT_EQ(19902, socket2->local_port());
}

TEST(UdpSocketTest, SendReceive) {
    // 接收方：绑定固定端口
    auto recv_socket = net::create_udp_socket("127.0.0.1", 19900);
    ASSERT_NE(nullptr, recv_socket);
    EXPECT_EQ(19900, recv_socket->local_port());

    // 发送方：绑定临时端口
    auto send_socket = net::create_udp_socket("127.0.0.1", 0);
    ASSERT_NE(nullptr, send_socket);
    ASSERT_TRUE(net::bind_udp_port(send_socket, "127.0.0.1", 0));

    const std::string payload = "Test message";
    send_socket->send_to("127.0.0.1", 19900, payload);

    // 使用 poll+recvfrom 实际读取，并断言收到的内容正确
    std::string received = udp_recv_once(recv_socket->native_handle(), 500);
    EXPECT_EQ(received, payload);
}

TEST(UdpSocketTest, SendReceiveMultipleMessages) {
    auto recv_socket = net::create_udp_socket("127.0.0.1", 19910);
    ASSERT_NE(nullptr, recv_socket);

    auto send_socket = net::create_udp_socket("127.0.0.1", 0);
    ASSERT_NE(nullptr, send_socket);
    ASSERT_TRUE(net::bind_udp_port(send_socket, "127.0.0.1", 0));

    const std::vector<std::string> messages = {"msg1", "msg2", "msg3"};
    for (const auto& m : messages) {
        send_socket->send_to("127.0.0.1", 19910, m);
    }

    for (const auto& expected : messages) {
        std::string got = udp_recv_once(recv_socket->native_handle(), 500);
        EXPECT_EQ(got, expected);
    }
}

TEST(UdpSocketTest, CloseCallbackReceivesValidPointer) {
    auto socket = net::create_udp_socket("127.0.0.1", 0);
    ASSERT_NE(nullptr, socket);

    bool callback_fired = false;
    net::UdpSocketPtr captured_ptr;
    socket->set_close_callback([&](const net::UdpSocketPtr& ptr) {
        callback_fired = true;
        captured_ptr = ptr;
    });

    // 正常 close：callback 应收到有效指针（non-null）
    socket->close();

    EXPECT_TRUE(callback_fired);
    // close() 后对象仍存在（socket 持有），指针应有效
    EXPECT_NE(nullptr, captured_ptr);
}
