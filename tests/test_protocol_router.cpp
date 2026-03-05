#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "chwell/service/protocol_router.h"
#include "chwell/protocol/message.h"

using namespace chwell;

namespace {

net::TcpConnectionPtr make_dummy_conn(std::uintptr_t tag) {
    return net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(tag),
        [](net::TcpConnection*) {});
}

}  // namespace

TEST(ProtocolRouterTest, RoutesToRegisteredHandler) {
    service::ProtocolRouterComponent router;

    bool called = false;
    protocol::Message received;

    router.register_handler(
        42, [&](const net::TcpConnectionPtr& /*conn*/, const protocol::Message& msg) {
            called = true;
            received = msg;
        });

    protocol::Message m1(42, std::string("hello"));
    protocol::Message m2(43, std::string("ignored"));

    auto d1 = protocol::serialize(m1);
    auto d2 = protocol::serialize(m2);

    std::vector<char> combined;
    combined.reserve(d1.size() + d2.size());
    combined.insert(combined.end(), d1.begin(), d1.end());
    combined.insert(combined.end(), d2.begin(), d2.end());

    auto conn = make_dummy_conn(1);
    router.on_message(conn, combined);

    ASSERT_TRUE(called);
    EXPECT_EQ(42u, received.cmd);
    std::string body(received.body.begin(), received.body.end());
    EXPECT_EQ("hello", body);
}

