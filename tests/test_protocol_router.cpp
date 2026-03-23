#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/protocol/message.h"
#include "chwell/protocol/parser.h"
#include "chwell/service/protocol_router.h"

using namespace chwell;

namespace {

// 创建一个 fd=-1 的空 TcpConnection，仅用作 parsers_ 的键，不会发起真实 I/O
net::TcpConnectionPtr make_dummy_conn() {
    return std::make_shared<net::TcpConnection>(net::TcpSocket());
}

// 构造单帧序列化数据
std::vector<char> make_frame(uint16_t cmd, const std::string& body) {
    return protocol::serialize(protocol::Message(cmd, body));
}

}  // namespace

// 1. 注册 handler 后，完整帧被正确路由并调用
TEST(ProtocolRouterTest, RoutesToRegisteredHandler) {
    service::ProtocolRouterComponent router;

    uint16_t received_cmd = 0;
    std::string received_body;
    router.register_handler(0x0001, [&](const net::TcpConnectionPtr&,
                                        const protocol::Message& msg) {
        received_cmd = msg.cmd;
        received_body = std::string(msg.body.begin(), msg.body.end());
    });

    auto conn = make_dummy_conn();
    router.on_message(conn, make_frame(0x0001, "hello"));

    EXPECT_EQ(received_cmd, 0x0001u);
    EXPECT_EQ(received_body, "hello");
}

// 2. 未注册的 cmd 不触发任何 handler，也不崩溃
TEST(ProtocolRouterTest, UnknownCmdDoesNotCrash) {
    service::ProtocolRouterComponent router;

    bool called = false;
    router.register_handler(0x0001, [&](const net::TcpConnectionPtr&,
                                        const protocol::Message&) { called = true; });

    auto conn = make_dummy_conn();
    ASSERT_NO_THROW(router.on_message(conn, make_frame(0x9999, "ignored")));
    EXPECT_FALSE(called);
}

// 3. 粘包：分两次 feed 半帧，仍能正确拼出完整消息并路由一次
TEST(ProtocolRouterTest, ReassemblesFragmentedMessage) {
    service::ProtocolRouterComponent router;

    int call_count = 0;
    router.register_handler(0x0002, [&](const net::TcpConnectionPtr&,
                                        const protocol::Message& msg) {
        ++call_count;
        EXPECT_EQ(std::string(msg.body.begin(), msg.body.end()), "fragment");
    });

    auto conn = make_dummy_conn();
    auto frame = make_frame(0x0002, "fragment");
    std::size_t half = frame.size() / 2;

    router.on_message(conn, {frame.begin(), frame.begin() + half});
    EXPECT_EQ(call_count, 0);  // 不完整，尚未触发

    router.on_message(conn, {frame.begin() + half, frame.end()});
    EXPECT_EQ(call_count, 1);  // 补齐后触发一次
}

// 4. 同一次 feed 中连续两条消息均被正确路由
TEST(ProtocolRouterTest, RoutesTwoMessagesInOneFeed) {
    service::ProtocolRouterComponent router;

    std::vector<uint16_t> cmds;
    auto record = [&](const net::TcpConnectionPtr&, const protocol::Message& msg) {
        cmds.push_back(msg.cmd);
    };
    router.register_handler(0x0010, record);
    router.register_handler(0x0011, record);

    auto conn = make_dummy_conn();
    auto f1 = make_frame(0x0010, "a");
    auto f2 = make_frame(0x0011, "b");
    std::vector<char> combined;
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());

    router.on_message(conn, combined);

    ASSERT_EQ(cmds.size(), 2u);
    EXPECT_EQ(cmds[0], 0x0010u);
    EXPECT_EQ(cmds[1], 0x0011u);
}

// 5. on_disconnect 清理 parser：残留数据被丢弃，handler 不再触发
TEST(ProtocolRouterTest, DisconnectCleansParserForConnection) {
    service::ProtocolRouterComponent router;

    int call_count = 0;
    router.register_handler(0x0003, [&](const net::TcpConnectionPtr&,
                                        const protocol::Message&) { ++call_count; });

    auto conn = make_dummy_conn();
    auto frame = make_frame(0x0003, "clean");

    // 只送前 2 字节，parser 中留有不完整数据
    router.on_message(conn, {frame.begin(), frame.begin() + 2});
    router.on_disconnect(conn);  // 断开：parser 被清除

    // 补发剩余数据，因 parser 已清理，不应触发 handler
    router.on_message(conn, {frame.begin() + 2, frame.end()});
    EXPECT_EQ(call_count, 0);
}

