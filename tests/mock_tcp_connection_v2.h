#pragma once

// 注意：本文件提供 MockConnectionFactoryV2（真实 MockTcpConnection 对象版本），
// mock_tcp_connection.h 提供 MockConnectionFactory（轻量指针整数版本）。
// 请勿在同一翻译单元内同时包含两个文件，以避免命名冲突。
// 如需两者，将引用改为对应的 V2 版本类名即可。

#include <atomic>
#include <functional>
#include <vector>
#include "chwell/net/tcp_connection.h"

namespace chwell {
namespace test {

// Mock TcpConnection 对象：持有真实堆内存，支持 send/close 记录
class MockTcpConnection {
public:
    explicit MockTcpConnection(int fd = 0) : fd_(fd), closed_(false) {}

    void start() {}
    void send(const std::vector<char>& data) {}
    void close() { closed_ = true; }

    void set_message_callback(const net::MessageCallback& cb) {}
    void set_close_callback(const net::ConnectionCallback& cb) {}

    int native_handle() const noexcept { return fd_; }
    bool is_closed() const noexcept { return closed_.load(); }

private:
    int fd_;
    std::atomic<bool> closed_;
};

// V2 工厂：创建的 TcpConnectionPtr 指向真实 MockTcpConnection 堆对象，
// 与 mock_tcp_connection.h 中的 MockConnectionFactory 不同，
// 命名为 MockConnectionFactoryV2 以消除 ODR 冲突。
class MockConnectionFactoryV2 {
public:
    static int next_id() {
        static std::atomic<int> counter{0};
        return ++counter;
    }

    static net::TcpConnectionPtr create() {
        int id = next_id();
        auto* mock_conn = new MockTcpConnection(id);
        return net::TcpConnectionPtr(
            reinterpret_cast<net::TcpConnection*>(mock_conn),
            [](net::TcpConnection* ptr) {
                delete reinterpret_cast<MockTcpConnection*>(ptr);
            });
    }

    static MockTcpConnection* unwrap(const net::TcpConnectionPtr& conn) {
        return reinterpret_cast<MockTcpConnection*>(conn.get());
    }
};

} // namespace test
} // namespace chwell
