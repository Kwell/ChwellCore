#include <gtest/gtest.h>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <vector>

#include "chwell/cluster/node_registry.h"
#include "chwell/discovery/service_discovery.h"
#include "chwell/cluster/node.h"
#include "chwell/redis/distributed_lock.h"
#include "chwell/redis/redis_client.h"
#include "chwell/loadbalance/consistent_hash.h"
#include "chwell/core/logger.h"

using namespace chwell;

namespace {

// ============================================
// NodeRegistry 一致性哈希路由测试
// ============================================

TEST(NodeRegistryTest, SelectNodeByHash_SingleNode) {
    cluster::NodeRegistry reg;
    reg.register_node("node1", "127.0.0.1", 9001, "game");

    cluster::NodeInfo info;
    EXPECT_TRUE(reg.select_node_by_hash("any_key", info, "game"));
    EXPECT_EQ("node1", info.node_id);
}

TEST(NodeRegistryTest, SelectNodeByHash_Distributes) {
    cluster::NodeRegistry reg;
    reg.register_node("node1", "127.0.0.1", 9001, "game");
    reg.register_node("node2", "127.0.0.1", 9002, "game");
    reg.register_node("node3", "127.0.0.1", 9003, "game");

    std::unordered_map<std::string, int> hit_count;
    for (int i = 0; i < 300; ++i) {
        std::string key = "client_" + std::to_string(i);
        cluster::NodeInfo info;
        ASSERT_TRUE(reg.select_node_by_hash(key, info, "game"));
        hit_count[info.node_id]++;
    }

    // Each node should receive at least some requests (consistent hash distributes)
    EXPECT_GE(hit_count["node1"], 1);
    EXPECT_GE(hit_count["node2"], 1);
    EXPECT_GE(hit_count["node3"], 1);
}

TEST(NodeRegistryTest, SelectNodeByHash_SameKeySameNode) {
    cluster::NodeRegistry reg;
    reg.register_node("node1", "127.0.0.1", 9001, "game");
    reg.register_node("node2", "127.0.0.1", 9002, "game");

    cluster::NodeInfo info1, info2;
    EXPECT_TRUE(reg.select_node_by_hash("stable_client", info1, "game"));
    EXPECT_TRUE(reg.select_node_by_hash("stable_client", info2, "game"));
    EXPECT_EQ(info1.node_id, info2.node_id); // same key -> same node
}

TEST(NodeRegistryTest, UnregisterNodeExcludesFromHash) {
    cluster::NodeRegistry reg;
    reg.register_node("node1", "127.0.0.1", 9001, "game");
    reg.register_node("node2", "127.0.0.1", 9002, "game");

    reg.unregister_node("node2");

    for (int i = 0; i < 50; ++i) {
        std::string key = "key_" + std::to_string(i);
        cluster::NodeInfo info;
        ASSERT_TRUE(reg.select_node_by_hash(key, info, "game"));
        EXPECT_EQ("node1", info.node_id); // only node1 is online
    }
}

TEST(NodeRegistryTest, NoNodesReturnsEmpty) {
    cluster::NodeRegistry reg;
    cluster::NodeInfo info;
    EXPECT_FALSE(reg.select_node_by_hash("key", info, "game"));
}

// ============================================
// ServiceDiscovery 过期清理测试
// ============================================

TEST(ServiceDiscoveryCleanupTest, BackgroundThreadClearsExpiredInstances) {
    // Use very short timeout (100ms) and cleanup interval (50ms) for test speed
    discovery::MemoryServiceDiscovery disc(100, 50);

    discovery::ServiceInstance inst;
    inst.service_id  = "svc";
    inst.instance_id = "inst1";
    inst.host        = "127.0.0.1";
    inst.port        = 8080;
    ASSERT_TRUE(disc.register_service(inst));

    // Initial heartbeat should make it alive
    EXPECT_TRUE(disc.is_alive("inst1"));

    // Wait longer than heartbeat timeout without sending heartbeat
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Background cleanup should have removed the expired instance
    auto instances = disc.discover_services("svc");
    EXPECT_EQ(0u, instances.size());
}

TEST(ServiceDiscoveryCleanupTest, HeartbeatKeepsInstanceAlive) {
    discovery::MemoryServiceDiscovery disc(200, 50);

    discovery::ServiceInstance inst;
    inst.service_id  = "svc";
    inst.instance_id = "inst1";
    inst.host        = "127.0.0.1";
    inst.port        = 8080;
    ASSERT_TRUE(disc.register_service(inst));

    // Send heartbeats to keep alive
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_TRUE(disc.heartbeat("inst1"));
    }

    // Should still be alive
    auto instances = disc.discover_services("svc");
    EXPECT_EQ(1u, instances.size());
}

// ============================================
// Node 心跳生命周期测试
// ============================================

TEST(NodeHeartbeatTest, NodeRegistersAndBeats) {
    discovery::MemoryServiceDiscovery disc(5000, 0); // no background cleanup for this test

    net::IoService io;
    cluster::Node node(io, "n1", "game", "127.0.0.1", 9001, 1);

    EXPECT_TRUE(node.start(&disc));
    EXPECT_TRUE(node.is_running());

    // Wait for at least one heartbeat
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    EXPECT_TRUE(disc.is_alive("n1"));
    auto instances = disc.discover_services("game");
    EXPECT_EQ(1u, instances.size());
}

TEST(NodeHeartbeatTest, NodeDeregistersOnStop) {
    discovery::MemoryServiceDiscovery disc(5000, 0);

    net::IoService io;
    cluster::Node node(io, "n2", "game", "127.0.0.1", 9002, 60);

    ASSERT_TRUE(node.start(&disc));
    node.stop();
    EXPECT_FALSE(node.is_running());

    auto instances = disc.discover_services("game");
    EXPECT_EQ(0u, instances.size());
}

// ============================================
// DistributedLock 单元测试（使用内存 RedisClient）
// ============================================

TEST(DistributedLockTest, TryLockSucceeds) {
    auto redis = std::make_shared<redis::RedisClient>();
    redis->connect();

    redis::DistributedLock lock(redis, "test:lock:basic", 30);
    EXPECT_TRUE(lock.try_lock());
    EXPECT_TRUE(lock.is_locked());
    EXPECT_TRUE(lock.unlock());
    EXPECT_FALSE(lock.is_locked());
}

TEST(DistributedLockTest, SecondLockFails) {
    auto redis = std::make_shared<redis::RedisClient>();
    redis->connect();

    redis::DistributedLock lock1(redis, "test:lock:contention", 30);
    redis::DistributedLock lock2(redis, "test:lock:contention", 30);

    EXPECT_TRUE(lock1.try_lock());
    EXPECT_FALSE(lock2.try_lock()); // same key, should fail

    lock1.unlock();
    EXPECT_TRUE(lock2.try_lock()); // now available
    lock2.unlock();
}

TEST(DistributedLockTest, GuardRAII) {
    auto redis = std::make_shared<redis::RedisClient>();
    redis->connect();

    {
        redis::DistributedLockGuard guard(redis, "test:lock:raii", 30, 1000);
        EXPECT_TRUE(guard.acquired());
        EXPECT_GT(guard.fencing_token(), 0u);
    }
    // After guard goes out of scope, lock should be released
    redis::DistributedLockGuard guard2(redis, "test:lock:raii", 30, 100);
    EXPECT_TRUE(guard2.acquired());
}

TEST(DistributedLockTest, FencingTokenIncreases) {
    auto redis = std::make_shared<redis::RedisClient>();
    redis->connect();

    redis::DistributedLock lock(redis, "test:lock:fencing", 30);

    EXPECT_TRUE(lock.try_lock());
    uint64_t token1 = lock.fencing_token();
    lock.unlock();

    EXPECT_TRUE(lock.try_lock());
    uint64_t token2 = lock.fencing_token();
    lock.unlock();

    EXPECT_GT(token2, token1);
}

// ============================================
// ConsistentHash 节点增减影响范围测试
// ============================================

TEST(ConsistentHashTest, MinimalRehashOnAdd) {
    loadbalance::ConsistentHashLoadBalancer ch(100);
    ch.add_instance("svc", "inst1");
    ch.add_instance("svc", "inst2");
    ch.add_instance("svc", "inst3");

    // Snapshot routing before adding inst4
    std::unordered_map<std::string, std::string> before;
    for (int i = 0; i < 100; ++i) {
        std::string key = "k" + std::to_string(i);
        std::string id;
        ch.select_instance("svc", key, id);
        before[key] = id;
    }

    ch.add_instance("svc", "inst4");

    int changed = 0;
    for (int i = 0; i < 100; ++i) {
        std::string key = "k" + std::to_string(i);
        std::string id;
        ch.select_instance("svc", key, id);
        if (id != before[key]) changed++;
    }

    // With 4 nodes, roughly 1/4 of keys should remap; allow generous margin
    EXPECT_LT(changed, 60);
}

} // namespace
