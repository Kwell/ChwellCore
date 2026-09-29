#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "chwell/cluster/node_registry.h"
#include "chwell/cluster/rpc_router.h"
#include "chwell/cluster/session_locator.h"

using namespace chwell::cluster;

namespace {

std::string node_of_resp(const std::vector<char>& resp) {
    auto it = std::find(resp.begin(), resp.end(), ':');
    return std::string(resp.begin(), it);
}

class MockTransport : public RpcTransport {
public:
    explicit MockTransport(std::string node_id) : node_id_(std::move(node_id)) {}

    bool call(std::uint16_t cmd, const std::vector<char>& request,
              std::vector<char>& response, int timeout_ms) override {
        (void)timeout_ms;
        ++calls;
        last_cmd = cmd;
        last_request = request;
        if (fail) return false;
        // 回显：节点 id + 原始请求
        response.assign(node_id_.begin(), node_id_.end());
        response.push_back(':');
        response.insert(response.end(), request.begin(), request.end());
        return true;
    }

    bool healthy() const override { return !fail; }

    std::string node_id_;
    bool fail = false;
    int calls = 0;
    std::uint16_t last_cmd = 0;
    std::vector<char> last_request;
};

struct FactoryState {
    std::unordered_map<std::string, std::shared_ptr<MockTransport>> created;
    int create_count = 0;
};

using FactoryStatePtr = std::shared_ptr<FactoryState>;

FactoryStatePtr make_factory() {
    return std::make_shared<FactoryState>();
}

RpcTransportFactory bind_factory(FactoryStatePtr st) {
    return [st](const NodeInfo& n) -> RpcTransportPtr {
        auto tp = std::make_shared<MockTransport>(n.node_id);
        st->created[n.node_id] = tp;
        ++st->create_count;
        return tp;
    };
}

std::shared_ptr<NodeRegistry> make_registry() {
    auto reg = std::make_shared<NodeRegistry>();
    reg->register_node("logic-1", "10.0.0.1", 9001, "logic");
    reg->register_node("logic-2", "10.0.0.2", 9001, "logic");
    reg->register_node("gate-1", "10.0.0.3", 8000, "gate");
    return reg;
}

}  // namespace

TEST(RpcRouterTest, ForwardSelectsNodeByHash) {
    auto reg = make_registry();
    RpcRouter router(reg);
    auto factory = make_factory();
    router.set_transport_factory(bind_factory(factory));

    std::vector<char> req = {'h', 'i'};
    std::vector<char> resp;
    ASSERT_TRUE(router.forward("logic", "player-42", 0x1001, req, resp));

    // 同一 key 恒定打到同一节点
    std::vector<char> resp2;
    ASSERT_TRUE(router.forward("logic", "player-42", 0x1001, req, resp2));
    EXPECT_EQ(resp, resp2);
    // 连接复用：只创建一条
    EXPECT_EQ(1, factory->create_count);
}

TEST(RpcRouterTest, ForwardToUnknownServiceFails) {
    auto reg = make_registry();
    RpcRouter router(reg);
    auto factory = make_factory();
    router.set_transport_factory(bind_factory(factory));

    std::vector<char> req;
    std::vector<char> resp;
    EXPECT_FALSE(router.forward("missing-service", "k", 1, req, resp));
}

TEST(RpcRouterTest, FailoverUsesAnotherNode) {
    auto reg = make_registry();
    RpcRouter router(reg);
    auto factory = make_factory();
    router.set_transport_factory(bind_factory(factory));
    router.set_failover_retries(1);

    std::vector<char> req = {'x'};
    std::vector<char> resp;
    ASSERT_TRUE(router.forward("logic", "k1", 0x20, req, resp));

    // 让当前节点变坏，再调用应 failover 到另一节点
    std::string used_node = node_of_resp(resp);
    factory->created[used_node]->fail = true;

    std::vector<char> resp2;
    ASSERT_TRUE(router.forward("logic", "k1", 0x20, req, resp2));
    std::string used_node2 = node_of_resp(resp2);
    EXPECT_NE(used_node, used_node2);
}

TEST(RpcRouterTest, ForwardToExplicitNode) {
    auto reg = make_registry();
    RpcRouter router(reg);
    auto factory = make_factory();
    router.set_transport_factory(bind_factory(factory));

    NodeInfo node;
    ASSERT_TRUE(reg->find_node("gate-1", node));
    std::vector<char> req = {'g'};
    std::vector<char> resp;
    ASSERT_TRUE(router.forward_to_node(node, 0x99, req, resp));
    EXPECT_EQ("gate-1", std::string(resp.begin(), resp.begin() + 6));
    EXPECT_EQ(0x99, factory->created["gate-1"]->last_cmd);
}

TEST(RpcRouterTest, UnhealthyTransportRecreated) {
    auto reg = make_registry();
    RpcRouter router(reg);
    auto factory = make_factory();
    router.set_transport_factory(bind_factory(factory));

    std::vector<char> req;
    std::vector<char> resp;
    ASSERT_TRUE(router.forward("logic", "k2", 1, req, resp));
    int before = factory->create_count;

    // drop 后重建
    std::string used_node = node_of_resp(resp);
    router.drop(used_node);
    ASSERT_TRUE(router.forward("logic", "k2", 1, req, resp));
    EXPECT_GT(factory->create_count, before);
}

TEST(SessionLocatorTest, BindLocateUnbind) {
    SessionLocator loc;
    SessionLocation missing;
    EXPECT_FALSE(loc.locate("s1", missing));

    ASSERT_TRUE(loc.bind("s1", "node-a", "logic", 1000));
    SessionLocation out;
    ASSERT_TRUE(loc.locate("s1", out));
    EXPECT_EQ("node-a", out.node_id);
    EXPECT_EQ("logic", out.node_type);
    EXPECT_EQ(1000, out.bind_time_ms);
    EXPECT_EQ("node-a", loc.node_of("s1"));

    ASSERT_TRUE(loc.unbind("s1"));
    EXPECT_EQ("", loc.node_of("s1"));
    EXPECT_EQ(0u, loc.count());
}

TEST(SessionLocatorTest, BindRejectsEmpty) {
    SessionLocator loc;
    EXPECT_FALSE(loc.bind("", "n1"));
    EXPECT_FALSE(loc.bind("s1", ""));
    EXPECT_EQ(0u, loc.count());
}

TEST(SessionLocatorTest, MigrateMovesSession) {
    SessionLocator loc;
    ASSERT_TRUE(loc.bind("s1", "node-a", "logic", 1));

    EXPECT_FALSE(loc.migrate("s1", "node-wrong", "node-b"));  // old 不匹配
    EXPECT_EQ("node-a", loc.node_of("s1"));

    ASSERT_TRUE(loc.migrate("s1", "node-a", "node-b", "logic", 2));
    EXPECT_EQ("node-b", loc.node_of("s1"));

    // old 为空时忽略来源检查
    ASSERT_TRUE(loc.migrate("s1", "", "node-c", "logic", 3));
    EXPECT_EQ("node-c", loc.node_of("s1"));

    EXPECT_FALSE(loc.migrate("nope", "node-c", "node-d"));
}

TEST(SessionLocatorTest, DropNodeClearsAllSessions) {
    SessionLocator loc;
    ASSERT_TRUE(loc.bind("s1", "n1"));
    ASSERT_TRUE(loc.bind("s2", "n1"));
    ASSERT_TRUE(loc.bind("s3", "n2"));
    EXPECT_EQ(2u, loc.count_on_node("n1"));

    EXPECT_EQ(2u, loc.drop_node("n1"));
    EXPECT_EQ(1u, loc.count());
    EXPECT_EQ("n2", loc.node_of("s3"));
}

TEST(SessionLocatorTest, RebindOverwrites) {
    SessionLocator loc;
    ASSERT_TRUE(loc.bind("s1", "n1", "logic", 1));
    ASSERT_TRUE(loc.bind("s1", "n2", "logic", 2));
    EXPECT_EQ(1u, loc.count());
    EXPECT_EQ("n2", loc.node_of("s1"));
}
