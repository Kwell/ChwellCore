#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <functional>

#if defined(CHWELL_USE_YAML)
#include <yaml-cpp/yaml.h>
#endif

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
    // 从 YAML 配置加载节点列表（静态发现）
    // 示例文件见 config/cluster.yaml
    bool load_from_yaml_file(const std::string& yaml_path) {
#if defined(CHWELL_USE_YAML)
        try {
            YAML::Node root = YAML::LoadFile(yaml_path);
            return load_from_yaml_node(root);
        } catch (const std::exception& e) {
            (void)e;
            return false;
        }
#else
        (void)yaml_path;
        return false;
#endif
    }

    // 从 YAML 字符串加载（便于测试）
    bool load_from_yaml_string(const std::string& yaml_content) {
#if defined(CHWELL_USE_YAML)
        try {
            YAML::Node root = YAML::Load(yaml_content);
            return load_from_yaml_node(root);
        } catch (const std::exception& e) {
            (void)e;
            return false;
        }
#else
        (void)yaml_content;
        return false;
#endif
    }

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

    // 一致性哈希选择节点：根据 key 在指定类型的在线节点中选择一个节点
    // 若 node_type 为空，则在所有在线节点中选择
    bool select_node_by_hash(const std::string& key,
                             NodeInfo& out,
                             const std::string& node_type = std::string()) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<const NodeInfo*> candidates;
        candidates.reserve(nodes_.size());
        for (const auto& pair : nodes_) {
            const NodeInfo& info = pair.second;
            if (!info.online) continue;
            if (!node_type.empty() && info.node_type != node_type) continue;
            candidates.push_back(&info);
        }
        if (candidates.empty()) {
            return false;
        }

        // 简单一致性哈希：对 key+node_id 计算 hash，选择 hash 最大的节点
        std::hash<std::string> hasher;
        std::size_t best_hash = 0;
        const NodeInfo* best = nullptr;
        for (const NodeInfo* info : candidates) {
            std::string combined = key;
            combined.append("#").append(info->node_id);
            std::size_t h = hasher(combined);
            if (!best || h > best_hash) {
                best_hash = h;
                best = info;
            }
        }
        if (!best) {
            return false;
        }
        out = *best;
        return true;
    }

private:
#if defined(CHWELL_USE_YAML)
    bool load_from_yaml_node(const YAML::Node& root) {
        if (!root["nodes"]) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        nodes_.clear();

        const YAML::Node& nodes = root["nodes"];
        for (const auto& n : nodes) {
            NodeInfo info;
            if (n["id"]) info.node_id = n["id"].as<std::string>();
            if (n["host"]) info.listen_addr = n["host"].as<std::string>();
            if (n["port"]) info.listen_port = n["port"].as<unsigned short>(0);
            if (n["type"]) info.node_type = n["type"].as<std::string>();
            info.online = true;
            if (!info.node_id.empty() && info.listen_port != 0) {
                nodes_[info.node_id] = info;
            }
        }
        return !nodes_.empty();
    }
#endif

    mutable std::mutex mutex_;
    std::unordered_map<std::string, NodeInfo> nodes_;
};

} // namespace cluster
} // namespace chwell
