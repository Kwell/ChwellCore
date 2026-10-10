#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "chwell/discovery/consul_discovery.h"

using namespace chwell::discovery;
using Json = nlohmann::json;

namespace {
Json service(const std::string& id = "game-1", const std::string& name = "game") {
    return {{"Node", {{"Address", "127.0.0.1"}}},
            {"Service", {{"ID", id}, {"Service", name}, {"Address", ""}, {"Port", 9201},
                         {"Meta", {{"incarnation", "first"}}}}},
            {"Checks", Json::array({{{"Status", "passing"}}})}};
}
}

TEST(ConsulDiscoveryTest, RegistrationUsesTTLAndHeartbeatAndDeregisterEscapeIdentity) {
    std::vector<std::string> requests;
    ConsulServiceDiscovery discovery({}, [&](const auto& method, const auto& path, const auto& body) {
        requests.push_back(method + " " + path);
        if (!body.empty()) {
            const auto data = Json::parse(body);
            EXPECT_EQ(data["Check"]["TTL"], "10s");
            EXPECT_EQ(data["Check"]["Status"], "passing");
            EXPECT_EQ(data["Check"]["DeregisterCriticalServiceAfter"], "60s");
            EXPECT_EQ(data["Check"]["CheckID"], "service:game /1");
            EXPECT_EQ(data["Meta"]["version"], "v1");
        }
        return ConsulHttpResponse{true, 200, ""};
    });
    ServiceInstance instance;
    instance.instance_id = "game /1"; instance.service_id = "game";
    instance.host = "127.0.0.1"; instance.port = 9201;
    instance.metadata["version"] = "v1";
    EXPECT_TRUE(discovery.register_service(instance));
    EXPECT_TRUE(discovery.heartbeat(instance.instance_id));
    EXPECT_TRUE(discovery.deregister_service(instance.instance_id));
    EXPECT_EQ(requests, (std::vector<std::string>{"PUT /v1/agent/service/register",
        "PUT /v1/agent/check/pass/service%3Agame%20%2F1", "PUT /v1/agent/service/deregister/game%20%2F1"}));
    instance.port = 0;
    EXPECT_FALSE(discovery.register_service(instance));
    EXPECT_EQ(requests.size(), 3u);
}

TEST(ConsulDiscoveryTest, SnapshotFailurePreservesOutputAndDoesNotPublishAnEmptyCluster) {
    ConsulHttpResponse result{true, 200, Json::array({service()}).dump()};
    ConsulServiceDiscovery discovery({}, [&](const auto&, const auto& path, const auto&) {
        EXPECT_EQ(path, "/v1/health/service/game?passing=true");
        return result;
    });
    int notifications = 0;
    discovery.add_listener("game", [&](const auto&, const auto&) { ++notifications; });
    std::vector<ServiceInstance> snapshot;
    ASSERT_TRUE(discovery.discover_services_checked("game", snapshot));
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_EQ(snapshot[0].host, "127.0.0.1");
    EXPECT_EQ(snapshot[0].metadata.at("incarnation"), "first");
    EXPECT_TRUE(snapshot[0].is_alive);
    EXPECT_EQ(notifications, 1);
    EXPECT_TRUE(discovery.discover_services_checked("game", snapshot));
    EXPECT_EQ(notifications, 1);
    for (const auto& bad : {ConsulHttpResponse{false, 0, ""}, ConsulHttpResponse{true, 403, "denied"},
                           ConsulHttpResponse{true, 200, "broken json"}, ConsulHttpResponse{true, 200, "{}"}}) {
        result = bad;
        EXPECT_FALSE(discovery.discover_services_checked("game", snapshot));
        EXPECT_EQ(snapshot.size(), 1u);
        EXPECT_EQ(notifications, 1);
        EXPECT_FALSE(discovery.last_error().empty());
    }
    result = {true, 200, "[]"};
    EXPECT_TRUE(discovery.discover_services_checked("game", snapshot));
    EXPECT_TRUE(snapshot.empty());
    EXPECT_EQ(notifications, 2);
}

TEST(ConsulDiscoveryTest, MalformedEndpointsChecksAndDuplicateIDsRejectTheWholeSnapshot) {
    std::string body;
    ConsulServiceDiscovery discovery({}, [&](const auto&, const auto&, const auto&) {
        return ConsulHttpResponse{true, 200, body};
    });
    auto port = service("bad"); port["Service"]["Port"] = 70000;
    auto fractional = service("bad"); fractional["Service"]["Port"] = 9201.5;
    auto critical = service("bad"); critical["Checks"][0]["Status"] = "critical";
    auto missing = service("bad"); missing.erase("Checks");
    auto metadata = service("bad"); metadata["Service"]["Meta"]["bad"] = 12;
    for (const auto& invalid : {port, fractional, critical, missing, metadata, service("id", "other")}) {
        body = Json::array({service(), invalid}).dump();
        std::vector<ServiceInstance> out;
        EXPECT_FALSE(discovery.discover_services_checked("game", out));
        EXPECT_TRUE(out.empty());
    }
    body = Json::array({service(), service()}).dump();
    std::vector<ServiceInstance> out;
    EXPECT_FALSE(discovery.discover_services_checked("game", out));
}

TEST(ConsulDiscoveryTest, ListenerExceptionsAndRemovalDoNotBreakSnapshots) {
    auto entry = service();
    ConsulServiceDiscovery discovery({}, [&](const auto&, const auto&, const auto&) {
        return ConsulHttpResponse{true, 200, Json::array({entry}).dump()};
    });
    std::vector<int> ports;
    discovery.add_listener("game", [](const auto&, const auto&) { throw std::runtime_error("listener"); });
    discovery.add_listener("game", [&](const auto&, const auto& node) {
        ports.push_back(node.port);
        discovery.remove_listener("game"); // Callback runs outside the backend mutex.
    });
    std::vector<ServiceInstance> out;
    EXPECT_TRUE(discovery.discover_services_checked("game", out));
    entry["Service"]["Port"] = 9202;
    EXPECT_TRUE(discovery.discover_services_checked("game", out));
    EXPECT_EQ(ports, (std::vector<int>{9201}));
}

TEST(ConsulDiscoveryTest, IdentityLookupQueriesCatalogAndPassingServices) {
    ConsulServiceDiscovery discovery({}, [&](const auto&, const auto& path, const auto&) {
        return ConsulHttpResponse{true, 200, path == "/v1/catalog/services"
            ? "{\"consul\":[],\"game\":[]}" : Json::array({service()}).dump()};
    });
    ServiceInstance out;
    EXPECT_TRUE(discovery.get_service_instance("game-1", out));
    EXPECT_EQ(out.port, 9201);
    EXPECT_FALSE(discovery.get_service_instance("missing", out));
    EXPECT_EQ(discovery.get_all_services(), (std::vector<std::string>{"consul", "game"}));
}

TEST(ConsulDiscoveryTest, InvalidConfigurationAndTransportExceptionsFailExplicitly) {
    ConsulConfig config;
    config.ttl_seconds = 0;
    EXPECT_THROW(ConsulServiceDiscovery(config, [](const auto&, const auto&, const auto&) { return ConsulHttpResponse{}; }), std::invalid_argument);
    ConsulServiceDiscovery discovery({}, [](const auto&, const auto&, const auto&) -> ConsulHttpResponse { throw std::runtime_error("no network"); });
    EXPECT_FALSE(discovery.heartbeat("id"));
    EXPECT_EQ(discovery.last_error(), "Consul transport threw");
}
