#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace chwell {
namespace loadbalance {

// 一致性哈希负载均衡器
// 使用ketama算法，确保节点增减时最小化数据迁移

class ConsistentHashLoadBalancer {
public:
    struct VirtualNode {
        std::string service_id;
        std::string instance_id;
        std::string node_id;  // 虚拟节点ID：instance_id:index
        uint32_t hash_value;
    };

    explicit ConsistentHashLoadBalancer(int virtual_nodes = 160);
    ~ConsistentHashLoadBalancer();

    // 添加实例（带权重）
    void add_instance(const std::string& service_id,
                    const std::string& instance_id,
                    int weight = 1);

    // 移除实例
    void remove_instance(const std::string& service_id,
                       const std::string& instance_id);

    // 选择实例
    bool select_instance(const std::string& service_id,
                         const std::string& key,
                         std::string& instance_id);

    // 获取所有实例列表
    std::vector<VirtualNode> get_all_instances(const std::string& service_id) const;

    // 清空
    void clear();

private:
    // 哈希函数（MurmurHash3 的简化版本）
    uint32_t hash(const std::string& key) const;

    // 获取虚拟节点ID
    std::string get_virtual_node_id(const std::string& instance_id, int index) const;

    int virtual_nodes_;  // 每个实例的虚拟节点数
    std::unordered_map<std::string, std::vector<VirtualNode>> service_instances_;  // service_id -> instances
    std::unordered_map<std::string, std::vector<VirtualNode>> service_ring_;  // service_id -> hash ring
};

} // namespace loadbalance
} // namespace chwell
