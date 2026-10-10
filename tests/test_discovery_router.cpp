#include <gtest/gtest.h>
#include "chwell/cluster/discovery_router.h"

using namespace chwell;
namespace {
class SnapshotDiscovery : public discovery::ServiceDiscovery {
public:
    std::vector<discovery::ServiceInstance> nodes;
    bool available = true;
    bool register_service(const discovery::ServiceInstance&) override { return false; }
    bool deregister_service(const std::string&) override { return false; }
    bool heartbeat(const std::string&) override { return false; }
    std::vector<discovery::ServiceInstance> discover_services(const std::string&) override { return nodes; }
    bool discover_services_checked(const std::string&, std::vector<discovery::ServiceInstance>& out) override {
        if (!available) return false;
        out = nodes; return true;
    }
    bool get_service_instance(const std::string&, discovery::ServiceInstance&) override { return false; }
    void add_listener(const std::string&, discovery::ServiceListener) override {}
    void remove_listener(const std::string&) override {}
    std::vector<std::string> get_all_services() override { return {}; }
};
discovery::ServiceInstance instance(const std::string& id, unsigned short port = 9201) {
    discovery::ServiceInstance out;
    out.instance_id = id; out.service_id = "game";
    out.host = "127.0.0.1"; out.port = port; out.is_alive = true;
    return out;
}
class EchoTransport : public cluster::RpcTransport {
public:
    std::string endpoint;
    bool succeeds = true;
    explicit EchoTransport(const cluster::NodeInfo& node) : endpoint(node.node_id + ":" + std::to_string(node.listen_port)) {}
    bool call(std::uint16_t, const std::vector<char>&, std::vector<char>& response, int) override {
        response.assign(endpoint.begin(), endpoint.end()); return succeeds;
    }
    bool healthy() const override { return true; }
};
class RoutingTest : public ::testing::Test {
protected:
    SnapshotDiscovery discovery;
    std::shared_ptr<cluster::NodeRegistry> registry = std::make_shared<cluster::NodeRegistry>();
    cluster::RpcRouter router{registry};
    cluster::SessionLocator sessions;
    cluster::DiscoveryRouter routes{discovery, registry, router, sessions, "game"};
    int connections = 0;
    void SetUp() override {
        router.set_transport_factory([this](const auto& node) {
            ++connections;
            return std::make_shared<EchoTransport>(node);
        });
        discovery.nodes = {instance("one"), instance("two", 9202)};
        ASSERT_TRUE(routes.refresh());
    }
};
}

TEST_F(RoutingTest, WithdrawalDropsRoutesTransportsAndOnlyAffectedLocalSessions) {
    ASSERT_TRUE(sessions.bind("alice", "one", "game"));
    ASSERT_TRUE(sessions.bind("bob", "two", "game"));
    std::vector<char> response;
    ASSERT_TRUE(routes.forward_session("alice", 1, {}, response));
    EXPECT_EQ(router.connection_count(), 1u);
    discovery.nodes.erase(discovery.nodes.begin());
    ASSERT_TRUE(routes.refresh());
    EXPECT_EQ(router.connection_count(), 0u);
    EXPECT_EQ(sessions.node_of("alice"), "");
    EXPECT_EQ(sessions.node_of("bob"), "two");
    EXPECT_FALSE(routes.forward_session("alice", 1, {}, response));
    EXPECT_TRUE(router.forward("game", "alice", 1, {}, response));
    EXPECT_EQ(std::string(response.begin(), response.end()), "two:9202");
}

TEST_F(RoutingTest, EndpointAndIncarnationChangesInvalidateCachesAndBindings) {
    std::vector<char> response;
    ASSERT_TRUE(sessions.bind("alice", "one"));
    ASSERT_TRUE(routes.forward_session("alice", 1, {}, response));
    discovery.nodes.front().port = 9301;
    ASSERT_TRUE(routes.refresh());
    EXPECT_EQ(sessions.count(), 0u);
    EXPECT_EQ(router.connection_count(), 0u);
    ASSERT_TRUE(sessions.bind("alice", "one"));
    ASSERT_TRUE(routes.forward_session("alice", 1, {}, response));
    EXPECT_EQ(std::string(response.begin(), response.end()), "one:9301");
    discovery.nodes.front().metadata["incarnation"] = "new-process";
    ASSERT_TRUE(routes.refresh());
    EXPECT_EQ(sessions.count(), 0u);
    EXPECT_EQ(router.connection_count(), 0u);
    EXPECT_EQ(connections, 2);
}

TEST_F(RoutingTest, UnavailableAndInvalidSnapshotsFailClosedAndRecover) {
    registry->register_node("static", "127.0.0.1", 9900, "other");
    ASSERT_TRUE(sessions.bind("alice", "one"));
    discovery.available = false;
    EXPECT_FALSE(routes.refresh());
    EXPECT_TRUE(registry->find_nodes_by_type("game").empty());
    EXPECT_EQ(sessions.count(), 0u);
    EXPECT_EQ(registry->find_nodes_by_type("other").size(), 1u);
    discovery.available = true;
    ASSERT_TRUE(routes.refresh());
    EXPECT_EQ(registry->find_nodes_by_type("game").size(), 2u);
    discovery.nodes.push_back(discovery.nodes.front());
    EXPECT_FALSE(routes.refresh());
    EXPECT_TRUE(registry->find_nodes_by_type("game").empty());
}

TEST_F(RoutingTest, StatefulSessionDoesNotReplayOnAnotherNodeAfterTransportFailure) {
    router.set_transport_factory([](const auto& node) {
        auto transport = std::make_shared<EchoTransport>(node);
        transport->succeeds = node.node_id != "one";
        return transport;
    });
    ASSERT_TRUE(sessions.bind("alice", "one"));
    std::vector<char> response{'u', 'n', 'c', 'h', 'a', 'n', 'g', 'e', 'd'};
    EXPECT_FALSE(routes.forward_session("alice", 1, {}, response));
    EXPECT_EQ(std::string(response.begin(), response.end()), "unchanged");
    EXPECT_EQ(sessions.node_of("alice"), "one");
}

TEST(NodeRegistryDiscoveryTest, InvalidAtomicSnapshotPreservesRegistryAndHashGroupsStayClean) {
    cluster::NodeRegistry registry;
    registry.register_node("one", "127.0.0.1", 9201, "old");
    registry.register_node("one", "127.0.0.1", 9202, "new");
    cluster::NodeInfo out;
    EXPECT_FALSE(registry.select_node_by_hash("key", out, "old"));
    EXPECT_TRUE(registry.select_node_by_hash("key", out, "new"));
    registry.unregister_node("one");
    EXPECT_FALSE(registry.select_node_by_hash("key", out));
    registry.register_node("one", "127.0.0.1", 9201, "game");
    EXPECT_FALSE(registry.replace_nodes_by_type("game", {cluster::NodeInfo{}}));
    EXPECT_TRUE(registry.find_node("one", out));
    EXPECT_EQ(out.listen_port, 9201);
}

TEST(RpcRouterDiscoveryTest, CacheTracksEndpointAndFactoryDoesNotRunUnderRouterMutex) {
    auto registry = std::make_shared<cluster::NodeRegistry>();
    cluster::RpcRouter router(registry);
    int connections = 0;
    router.set_transport_factory([&](const auto& node) {
        ++connections;
        EXPECT_LE(router.connection_count(), 1u); // Re-entry must not deadlock.
        return std::make_shared<EchoTransport>(node);
    });
    cluster::NodeInfo node;
    node.node_id = "one"; node.listen_addr = "127.0.0.1"; node.listen_port = 9201;
    std::vector<char> response;
    ASSERT_TRUE(router.forward_to_node(node, 1, {}, response));
    ASSERT_TRUE(router.forward_to_node(node, 1, {}, response));
    EXPECT_EQ(connections, 1);
    node.listen_port = 9202;
    ASSERT_TRUE(router.forward_to_node(node, 1, {}, response));
    EXPECT_EQ(connections, 2);
    EXPECT_EQ(std::string(response.begin(), response.end()), "one:9202");
}
