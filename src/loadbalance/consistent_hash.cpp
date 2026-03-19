#include "chwell/loadbalance/consistent_hash.h"

#include <algorithm>
#include <random>

namespace chwell {
namespace loadbalance {

// 使用 FNV-1a 哈希（比 MurmurHash3 简化，但足够分布均匀）
uint32_t ConsistentHashLoadBalancer::hash(const std::string& key) const {
    uint32_t h = 2166136261u;  // FNV offset basis

    for (char c : key) {
        h ^= static_cast<uint32_t>(static_cast<uint8_t>(c));
        h *= 16777619u;  // FNV prime
    }

    return h;
}

std::string ConsistentHashLoadBalancer::get_virtual_node_id(
    const std::string& instance_id, int index) const {
    return instance_id + "#" + std::to_string(index);
}

ConsistentHashLoadBalancer::ConsistentHashLoadBalancer(int virtual_nodes)
    : virtual_nodes_(virtual_nodes) {
}

ConsistentHashLoadBalancer::~ConsistentHashLoadBalancer() {
}

void ConsistentHashLoadBalancer::add_instance(
    const std::string& service_id,
    const std::string& instance_id,
    int weight) {

    // 根据权重调整虚拟节点数
    int node_count = virtual_nodes_ * weight;

    for (int i = 0; i < node_count; ++i) {
        std::string virtual_id = get_virtual_node_id(instance_id, i);
        uint32_t h = hash(virtual_id);

        VirtualNode vnode;
        vnode.service_id = service_id;
        vnode.instance_id = instance_id;
        vnode.node_id = virtual_id;
        vnode.hash_value = h;

        service_instances_[service_id].push_back(vnode);
        service_ring_[service_id].push_back(vnode);
    }

    // 按哈希值排序
    std::sort(service_ring_[service_id].begin(), service_ring_[service_id].end(),
        [](const VirtualNode& a, const VirtualNode& b) {
            return a.hash_value < b.hash_value;
        });
}

void ConsistentHashLoadBalancer::remove_instance(
    const std::string& service_id,
    const std::string& instance_id) {

    // 从实例列表中移除
    auto it = service_instances_.find(service_id);
    if (it == service_instances_.end()) {
        return;
    }

    auto& instances = it->second;
    instances.erase(
        std::remove_if(instances.begin(), instances.end(),
            [&instance_id](const VirtualNode& vnode) {
                return vnode.instance_id == instance_id;
            }),
        instances.end());

    // 从哈希环中移除
    auto ring_it = service_ring_.find(service_id);
    if (ring_it != service_ring_.end()) {
        auto& ring = ring_it->second;
        ring.erase(
            std::remove_if(ring.begin(), ring.end(),
                [&instance_id](const VirtualNode& vnode) {
                    return vnode.instance_id == instance_id;
                }),
            ring.end());
    }
}

bool ConsistentHashLoadBalancer::select_instance(
    const std::string& service_id,
    const std::string& key,
    std::string& instance_id) {

    auto it = service_ring_.find(service_id);
    if (it == service_ring_.end() || it->second.empty()) {
        return false;
    }

    const auto& ring = it->second;
    uint32_t h = hash(key);

    // 二分查找第一个 >= h 的节点
    auto it2 = std::lower_bound(ring.begin(), ring.end(),
        VirtualNode{service_id, "", "", h},
        [](const VirtualNode& a, const VirtualNode& b) {
            return a.hash_value < b.hash_value;
        });

    // 如果没找到（h 大于所有节点），取第一个
    if (it2 == ring.end()) {
        it2 = ring.begin();
    }

    instance_id = it2->instance_id;
    return true;
}

std::vector<ConsistentHashLoadBalancer::VirtualNode>
ConsistentHashLoadBalancer::get_all_instances(const std::string& service_id) const {

    auto it = service_instances_.find(service_id);
    if (it == service_instances_.end()) {
        return {};
    }
    return it->second;
}

void ConsistentHashLoadBalancer::clear() {
    service_instances_.clear();
    service_ring_.clear();
}

} // namespace loadbalance
} // namespace chwell
