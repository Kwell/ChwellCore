#pragma once

#include <vector>
#include <functional>
#include <atomic>
#include "chwell/net/tcp_connection.h"

namespace chwell {
namespace test {

// Mock TcpConnection 类，用于测试
class MockTcpConnection {
public:
    explicit MockTcpConnection(int fd = 0) : fd_(fd), closed_(false) {}

    void start() {}
    void send(const std::vector<char>& data) {}
    void close() { closed_ = true; }

    void set_message_callback(const net::MessageCallback& cb) {}
    void set_close_callback(const net::ConnectionCallback& cb) {}

    int native_handle() const noexcept { return fd_; }

private:
    int fd_;
    std::atomic<bool> closed_;
};

// Mock TcpConnection 工厂
class MockConnectionFactory {
public:
    static int next_id() {
        static std::atomic<int> counter{0};
        return ++counter;
    }

    // 创建 mock 连接
    static net::TcpConnectionPtr create() {
        int id = next_id();
        auto* mock_conn = new MockTcpConnection(id);

        return net::TcpConnectionPtr(
            reinterpret_cast<net::TcpConnection*>(mock_conn),
            [](net::TcpConnection* ptr) {
                auto* mock = reinterpret_cast<MockTcpConnection*>(ptr);
                delete mock;
            });
    }
};

} // namespace test
} // namespace chwell
