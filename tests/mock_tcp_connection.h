#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>
#include "chwell/net/tcp_connection.h"

namespace chwell {
namespace test {

// 连接 ID 包装器，用于测试
// 简单地使用 int 作为连接 ID，避免虚拟连接的指针问题
struct MockConnId {
    int id;
    MockConnId(int i = 0) : id(i) {}
    operator int() const { return id; }
    bool operator==(const MockConnId& other) const { return id == other.id; }
    bool operator<(const MockConnId& other) const { return id < other.id; }
};

// Mock 连接工厂
class MockConnectionFactory {
public:
    static int next_id() {
        static std::atomic<int> counter{1000};  // 从 1000 开始，避免与真实指针冲突
        return ++counter;
    }

    // 创建虚拟连接（返回 TcpConnectionPtr，内部使用 MockConnId）
    static net::TcpConnectionPtr create() {
        int id = next_id();
        return net::TcpConnectionPtr(
            reinterpret_cast<net::TcpConnection*>(static_cast<uintptr_t>(id)),
            [](net::TcpConnection*) {
                // 空删除器，不释放内存
            });
    }

    // 从 TcpConnectionPtr 获取连接 ID
    static int get_id(const net::TcpConnectionPtr& conn) {
        return static_cast<int>(reinterpret_cast<uintptr_t>(conn.get()));
    }
};

} // namespace test
} // namespace chwell
