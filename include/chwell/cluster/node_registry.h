#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "chwell/cluster/node.h"

namespace chwell {
namespace cluster {

// 节点信息
struct NodeInfo {
    std::string node_id;
    std::string listen_addr;
    unsigned short listen_port;
    std::string node_type; // 例如："gateway", "logic", "room"
    bool online;

    NodeInfo() : listen_port(0), online(false) {}
};

// 节点注册表：管理集群中所有节点的注册与发现
class NodeRegistry {
public:
    // 注册节点
    void register_node(const std::string& node_id,
                      const std::string& listen_addr,
                      unsigned short listen_port,
                      const std::string& node_type = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        NodeInfo& info = nodes_[node_id];
        info.node_id = node_id;
        info.listen_addr = listen_addr;
        info.listen_port = listen_port;
        info.node_type = node_type;
        info.online = true;
    }

    // 注销节点
    void unregister_node(const std::string& node_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodes_.find(node_id);
        if (it != nodes_.end()) {
            it->second.online = false;
        }
    }

    // 查找节点
    bool find_node(const std::string& node_id, NodeInfo& info) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodes_.find(node_id);
        if (it != nodes_.end() && it->second.online) {
            info = it->second;
            return true;
        }
        return false;
    }

    // 按类型查找所有在线节点
    std::vector<NodeInfo> find_nodes_by_type(const std::string& node_type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<NodeInfo> result;
        for (const auto& pair : nodes_) {
            if (pair.second.online && 
                (node_type.empty() || pair.second.node_type == node_type)) {
                result.push_back(pair.second);
            }
        }
        return result;
    }

    // 获取所有在线节点
    std::vector<NodeInfo> get_all_nodes() const {
        return find_nodes_by_type("");
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, NodeInfo> nodes_;
};

} // namespace cluster
} // namespace chwell
