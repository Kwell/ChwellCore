#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <algorithm>

#include "chwell/loadbalance/consistent_hash.h"
#include "chwell/core/logger.h"

using namespace chwell;

// ============================================
// 一致性哈希负载均衡器单元测试
// ============================================

TEST(ConsistentHashTest, AddAndSelectInstances) {
    loadbalance::ConsistentHashLoadBalancer lb(10);  // 每个实例10个虚拟节点

    lb.add_instance("test_service", "instance_1", 1);
    lb.add_instance("test_service", "instance_2", 1);
    lb.add_instance("test_service", "instance_3", 1);

    std::string instance_id;
    EXPECT_TRUE(lb.select_instance("test_service", "user123", instance_id));

    // 验证选择的是有效的实例
    auto instances = lb.get_all_instances("test_service");
    bool found = std::any_of(instances.begin(), instances.end(),
        [&instance_id](const auto& vnode) {
            return vnode.instance_id == instance_id;
        });
    EXPECT_TRUE(found);
}

TEST(ConsistentHashTest, RemoveInstance) {
    loadbalance::ConsistentHashLoadBalancer lb(10);

    lb.add_instance("test_service", "instance_1", 1);
    lb.add_instance("test_service", "instance_2", 1);

    std::string instance_id;
    lb.select_instance("test_service", "user123", instance_id);

    // 移除一个实例
    lb.remove_instance("test_service", instance_id);

    // 验证实例已移除
    auto instances = lb.get_all_instances("test_service");
    bool found = std::any_of(instances.begin(), instances.end(),
        [&instance_id](const auto& vnode) {
            return vnode.instance_id == instance_id;
        });
    EXPECT_FALSE(found);
}

TEST(ConsistentHashTest, WeightedInstances) {
    loadbalance::ConsistentHashLoadBalancer lb(200);  // 增加虚拟节点数

    lb.add_instance("test_service", "instance_1", 3);
    lb.add_instance("test_service", "instance_2", 1);

    // 统计选择次数（增加样本数）
    std::unordered_map<std::string, int> counts;
    for (int i = 0; i < 1000; ++i) {
        std::string key = "user" + std::to_string(i);
        std::string instance_id;
        lb.select_instance("test_service", key, instance_id);
        counts[instance_id]++;
    }

    // instance_1 应该被选择约 75% 次（范围放宽到 50-90%）
    int count_1 = counts["instance_1"];
    int count_2 = counts["instance_2"];

    EXPECT_GE(count_1, 500);  // 至少 50%
    EXPECT_LE(count_1, 900);  // 最多 90%
}

TEST(ConsistentHashTest, ConsistentSelection) {
    loadbalance::ConsistentHashLoadBalancer lb(10);

    lb.add_instance("test_service", "instance_1", 1);
    lb.add_instance("test_service", "instance_2", 1);

    std::string instance_id1;
    std::string instance_id2;
    std::string instance_id3;

    // 相同的key应该选择相同的实例
    lb.select_instance("test_service", "user123", instance_id1);
    lb.select_instance("test_service", "user123", instance_id2);
    lb.select_instance("test_service", "user123", instance_id3);

    EXPECT_EQ(instance_id1, instance_id2);
    EXPECT_EQ(instance_id2, instance_id3);
}

TEST(ConsistentHashTest, EmptyService) {
    loadbalance::ConsistentHashLoadBalancer lb(10);

    std::string instance_id;
    EXPECT_FALSE(lb.select_instance("test_service", "user123", instance_id));
}

TEST(ConsistentHashTest, MultipleServices) {
    loadbalance::ConsistentHashLoadBalancer lb(10);

    lb.add_instance("service_a", "instance_1", 1);
    lb.add_instance("service_a", "instance_2", 1);
    lb.add_instance("service_b", "instance_1", 1);
    lb.add_instance("service_b", "instance_2", 1);

    std::string instance_id_a, instance_id_b;
    lb.select_instance("service_a", "user123", instance_id_a);
    lb.select_instance("service_b", "user123", instance_id_b);

    // 验证不同服务独立
    auto instances_a = lb.get_all_instances("service_a");
    auto instances_b = lb.get_all_instances("service_b");

    EXPECT_EQ(20u, instances_a.size());  // 2 instances * 10 vnodes
    EXPECT_EQ(20u, instances_b.size());
}
