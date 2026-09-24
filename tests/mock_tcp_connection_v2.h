#pragma once

// 注意：本文件提供 MockConnectionFactoryV2（TcpConnection 子类版本），
// mock_tcp_connection.h 提供 MockConnectionFactory（轻量指针整数版本）。
// 请勿在同一翻译单元内同时包含两个文件，以避免命名冲突。

#include <atomic>
#include <memory>
#include <vector>
#include "chwell/net/tcp_connection.h"

namespace chwell {
namespace test {

// 真正的 TcpConnection 子类：conn_id()/成员布局均合法，
// 可安全传给 SessionManager 等会读取 conn_id() 的组件。
class MockTcpConnection : public net::TcpConnection {
public:
    explicit MockTcpConnection(int tag = 0)
        : net::TcpConnection(net::TcpSocket()), tag_(tag) {}

    void start() override {}
    void send(const std::vector<char>& data) override { (void)data; }
    void send(std::string_view data) override { (void)data; }
    void close() override { closed_.store(true, std::memory_order_relaxed); }

    int native_handle() const noexcept override { return tag_; }
    bool is_closed() const noexcept { return closed_.load(std::memory_order_relaxed); }

private:
    int tag_;
    std::atomic<bool> closed_{false};
};

class MockConnectionFactoryV2 {
public:
    static int next_id() {
        static std::atomic<int> counter{0};
        return ++counter;
    }

    static net::TcpConnectionPtr create() {
        // make_shared 会正确初始化 enable_shared_from_this
        return std::make_shared<MockTcpConnection>(next_id());
    }

    static MockTcpConnection* unwrap(const net::TcpConnectionPtr& conn) {
        return static_cast<MockTcpConnection*>(conn.get());
    }
};

} // namespace test
} // namespace chwell
