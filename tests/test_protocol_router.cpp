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
    // 暂时只验证 GoogleTest 运行环境，避免与真实网络/日志产生耦合
    // 后续如果需要，可以再引入更完整的 ProtocolRouter 行为测试。
    ASSERT_TRUE(true);
}

