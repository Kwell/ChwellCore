#include <gtest/gtest.h>

#include "chwell/discovery/service_discovery.h"
#include "chwell/loadbalance/load_balancer.h"
#include "chwell/core/logger.h"

using namespace chwell;

namespace {

// ============================================
// 服务发现单元测试
// ============================================

TEST(ServiceDiscoveryTest, RegisterService) {
    discovery::MemoryServiceDiscovery discovery(30000);

    discovery::ServiceInstance instance;
    instance.service_id = "test_service";
    instance.instance_id = "instance_1";
    instance.host = "127.0.0.1";
    instance.port = 8080;
    instance.metadata["version"] = "1.0.0";

    EXPECT_TRUE(discovery.register_service(instance));
    EXPECT_TRUE(discovery.is_alive("instance_1"));

    auto instances = discovery.discover_services("test_service");
    EXPECT_EQ(1, instances.size());
    EXPECT_EQ("instance_1", instances[0].instance_id);
}

TEST(ServiceDiscoveryTest, DeregisterService) {
    discovery::MemoryServiceDiscovery discovery(30000);

    discovery::ServiceInstance instance;
    instance.service_id = "test_service";
    instance.instance_id = "instance_1";
    instance.host = "127.0.0.1";
    instance.port = 8080;

    EXPECT_TRUE(discovery.register_service(instance));
    EXPECT_TRUE(discovery.deregister_service("instance_1"));

    auto instances = discovery.discover_services("test_service");
    EXPECT_EQ(0, instances.size());
}

TEST(ServiceDiscoveryTest, Heartbeat) {
    discovery::MemoryServiceDiscovery discovery(30000);

    discovery::ServiceInstance instance;
    instance.service_id = "test_service";
    instance.instance_id = "instance_1";
    instance.host = "127.0.0.1";
    instance.port = 8080;

    EXPECT_TRUE(discovery.register_service(instance));

    // 更新心跳
    EXPECT_TRUE(discovery.heartbeat("instance_1"));

    discovery::ServiceInstance out;
    EXPECT_TRUE(discovery.get_service_instance("instance_1", out));
    EXPECT_EQ("instance_1", out.instance_id);
}

TEST(ServiceDiscoveryTest, DiscoverServices) {
    discovery::MemoryServiceDiscovery discovery(30000);

    // 注册多个实例
    discovery::ServiceInstance instance1;
    instance1.service_id = "test_service";
    instance1.instance_id = "instance_1";
    instance1.host = "127.0.0.1";
    instance1.port = 8080;

    discovery::ServiceInstance instance2;
    instance2.service_id = "test_service";
    instance2.instance_id = "instance_2";
    instance2.host = "127.0.0.1";
    instance2.port = 8081;

    discovery::ServiceInstance instance3;
    instance3.service_id = "other_service";
    instance3.instance_id = "instance_3";
    instance3.host = "127.0.0.1";
    instance3.port = 8082;

    EXPECT_TRUE(discovery.register_service(instance1));
    EXPECT_TRUE(discovery.register_service(instance2));
    EXPECT_TRUE(discovery.register_service(instance3));

    // 查询 test_service
    auto instances = discovery.discover_services("test_service");
    EXPECT_EQ(2, instances.size());

    // 查询 other_service
    auto instances2 = discovery.discover_services("other_service");
    EXPECT_EQ(1, instances2.size());
}

TEST(ServiceDiscoveryTest, GetAllServices) {
    discovery::MemoryServiceDiscovery discovery(30000);

    discovery::ServiceInstance instance1;
    instance1.service_id = "service_1";
    instance1.instance_id = "instance_1";
    instance1.host = "127.0.0.1";
    instance1.port = 8080;

    discovery::ServiceInstance instance2;
    instance2.service_id = "service_2";
    instance2.instance_id = "instance_2";
    instance2.host = "127.0.0.1";
    instance2.port = 8081;

    EXPECT_TRUE(discovery.register_service(instance1));
    EXPECT_TRUE(discovery.register_service(instance2));

    auto services = discovery.get_all_services();
    EXPECT_EQ(2, services.size());
}

TEST(ServiceDiscoveryTest, ServiceListener) {
    discovery::MemoryServiceDiscovery discovery(30000);

    std::string notified_instance_id;
    discovery.add_listener("test_service", [&](const std::string& service_id, const discovery::ServiceInstance& instance) {
        notified_instance_id = instance.instance_id;
    });

    discovery::ServiceInstance instance;
    instance.service_id = "test_service";
    instance.instance_id = "instance_1";
    instance.host = "127.0.0.1";
    instance.port = 8080;

    EXPECT_TRUE(discovery.register_service(instance));

    // 检查是否通知到监听器
    EXPECT_EQ("instance_1", notified_instance_id);

    discovery.remove_listener("test_service");
}

// ============================================
// 负载均衡单元测试
// ============================================

TEST(LoadBalancerTest, RoundRobinSelect) {
    auto discovery = std::make_shared<discovery::MemoryServiceDiscovery>(30000);

    // 注册多个实例
    discovery::ServiceInstance instance1;
    instance1.service_id = "test_service";
    instance1.instance_id = "instance_1";
    instance1.host = "127.0.0.1";
    instance1.port = 8080;

    discovery::ServiceInstance instance2;
    instance2.service_id = "test_service";
    instance2.instance_id = "instance_2";
    instance2.host = "127.0.0.1";
    instance2.port = 8081;

    discovery->register_service(instance1);
    discovery->register_service(instance2);

    loadbalance::RoundRobinLoadBalancer lb(discovery);

    // 连续选择，应该轮询
    discovery::ServiceInstance out1;
    discovery::ServiceInstance out2;
    discovery::ServiceInstance out3;

    EXPECT_TRUE(lb.select_instance("test_service", out1));
    EXPECT_TRUE(lb.select_instance("test_service", out2));
    EXPECT_TRUE(lb.select_instance("test_service", out3));

    // 应该轮询
    EXPECT_NE(out1.instance_id, out2.instance_id);
    EXPECT_EQ(out1.instance_id, out3.instance_id);
}

TEST(LoadBalancerTest, RandomSelect) {
    auto discovery = std::make_shared<discovery::MemoryServiceDiscovery>(30000);

    discovery::ServiceInstance instance1;
    instance1.service_id = "test_service";
    instance1.instance_id = "instance_1";
    instance1.host = "127.0.0.1";
    instance1.port = 8080;

    discovery::ServiceInstance instance2;
    instance2.service_id = "test_service";
    instance2.instance_id = "instance_2";
    instance2.host = "127.0.0.1";
    instance2.port = 8081;

    discovery->register_service(instance1);
    discovery->register_service(instance2);

    loadbalance::RandomLoadBalancer lb(discovery);

    // 选择多次，应该都是随机的
    std::unordered_set<std::string> selected;
    for (int i = 0; i < 100; ++i) {
        discovery::ServiceInstance out;
        lb.select_instance("test_service", out);
        selected.insert(out.instance_id);
    }

    // 应该至少选择过两个实例
    EXPECT_GE(selected.size(), 2);
}

TEST(LoadBalancerTest, WeightedRoundRobinSelect) {
    auto discovery = std::make_shared<discovery::MemoryServiceDiscovery>(30000);

    discovery::ServiceInstance instance1;
    instance1.service_id = "test_service";
    instance1.instance_id = "instance_1";
    instance1.host = "127.0.0.1";
    instance1.port = 8080;

    discovery::ServiceInstance instance2;
    instance2.service_id = "test_service";
    instance2.instance_id = "instance_2";
    instance2.host = "127.0.0.1";
    instance2.port = 8081;

    discovery->register_service(instance1);
    discovery->register_service(instance2);

    loadbalance::WeightedRoundRobinLoadBalancer lb(discovery);

    // 设置权重：instance_1 权重 3，instance_2 权重 1
    lb.set_weight("instance_1", 3);
    lb.set_weight("instance_2", 1);

    // 选择多次，检查权重是否生效
    std::unordered_map<std::string, int> count;
    for (int i = 0; i < 100; ++i) {
        discovery::ServiceInstance out;
        lb.select_instance("test_service", out);
        count[out.instance_id]++;
    }

    // instance_1 的选择次数应该比 instance_2 多
    EXPECT_GT(count["instance_1"], count["instance_2"]);

    // instance_1 的选择次数应该约为 75% (3/4)
    EXPECT_GE(count["instance_1"], 60);
    EXPECT_LE(count["instance_1"], 90);
}

TEST(LoadBalancerTest, LoadBalancerStrategy) {
    auto discovery = std::make_shared<discovery::MemoryServiceDiscovery>(30000);

    discovery::ServiceInstance instance1;
    instance1.service_id = "test_service";
    instance1.instance_id = "instance_1";
    instance1.host = "127.0.0.1";
    instance1.port = 8080;

    discovery->register_service(instance1);

    loadbalance::RoundRobinLoadBalancer lb(discovery);

    EXPECT_EQ(loadbalance::LoadBalanceStrategy::ROUND_ROBIN, lb.get_strategy());

    lb.set_strategy(loadbalance::LoadBalanceStrategy::RANDOM);
    EXPECT_EQ(loadbalance::LoadBalanceStrategy::RANDOM, lb.get_strategy());
}

TEST(LoadBalancerTest, LoadBalancerSetWeight) {
    auto discovery = std::make_shared<discovery::MemoryServiceDiscovery>(30000);

    discovery::ServiceInstance instance1;
    instance1.service_id = "test_service";
    instance1.instance_id = "instance_1";
    instance1.host = "127.0.0.1";
    instance1.port = 8080;

    discovery->register_service(instance1);

    loadbalance::RoundRobinLoadBalancer lb(discovery);

    lb.set_weight("instance_1", 10);
    EXPECT_EQ(10, lb.get_weight("instance_1"));

    lb.set_weight("instance_1", 20);
    EXPECT_EQ(20, lb.get_weight("instance_1"));
}

TEST(LoadBalancerTest, LoadBalancerUpdateInstances) {
    auto discovery = std::make_shared<discovery::MemoryServiceDiscovery>(30000);

    discovery::ServiceInstance instance1;
    instance1.service_id = "test_service";
    instance1.instance_id = "instance_1";
    instance1.host = "127.0.0.1";
    instance1.port = 8080;

    discovery->register_service(instance1);

    discovery::ServiceInstance instance2;
    instance2.service_id = "test_service";
    instance2.instance_id = "instance_2";
    instance2.host = "127.0.0.1";
    instance2.port = 8081;

    loadbalance::RoundRobinLoadBalancer lb(discovery);

    std::vector<discovery::ServiceInstance> instances;
    instances.push_back(instance1);
    instances.push_back(instance2);

    lb.update_instances(instances);

    // 应该能够选择到两个实例
    discovery::ServiceInstance out1, out2;
    EXPECT_TRUE(lb.select_instance("test_service", out1));
    EXPECT_TRUE(lb.select_instance("test_service", out2));
}

} // namespace
