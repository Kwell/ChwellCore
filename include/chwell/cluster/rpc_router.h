#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <atomic>

#include "chwell/cluster/node_registry.h"

namespace chwell {
namespace cluster {

// ========== 跨服 RPC 路由 ==========
//
// RpcRouter：按 service_type + 路由 key 做一致性哈希选节点，把命令透传到目标进程。
// 连接层通过 RpcTransport 抽象注入：生产环境绑定 RpcClient 连接池，测试注入 Mock。
//
// 典型用法：
//   RpcRouter router(registry);
//   router.set_transport_factory([](const NodeInfo& n) { return make_rpc_client(n); });
//   router.forward("logic", player_id, cmd, payload, resp);

class RpcTransport {
public:
    virtual ~RpcTransport() = default;
    // 同步一次调用；ok=false 表示连接失败/超时
    virtual bool call(std::uint16_t cmd, const std::vector<char>& request,
                      std::vector<char>& response, int timeout_ms) = 0;
    virtual bool healthy() const = 0;
};

using RpcTransportPtr = std::shared_ptr<RpcTransport>;
using RpcTransportFactory = std::function<RpcTransportPtr(const NodeInfo&)>;

class RpcRouter {
public:
    explicit RpcRouter(std::shared_ptr<NodeRegistry> registry)
        : registry_(std::move(registry)) {}

    void set_transport_factory(RpcTransportFactory factory) {
        std::lock_guard<std::mutex> lock(mu_);
        factory_ = std::move(factory);
        transports_.clear();
    }

    // 路由并透传一次调用。返回 true 表示目标节点处理完成（响应在 out 中）。
    // 节点失败时最多重试 failover_retries 个不同节点。
    // Only enable retries for replay-safe requests; stateful commands must use
    // forward_to_node/DiscoveryRouter::forward_session or disable retries.
    bool forward(const std::string& service_type,
                 const std::string& route_key,
                 std::uint16_t cmd,
                 const std::vector<char>& request,
                 std::vector<char>& response,
                 int timeout_ms = 2000) {
        return forward_ex(service_type, route_key, cmd, request, response,
                          timeout_ms, failover_retries_.load());
    }

    bool forward_ex(const std::string& service_type,
                    const std::string& route_key,
                    std::uint16_t cmd,
                    const std::vector<char>& request,
                    std::vector<char>& response,
                    int timeout_ms,
                    int failover_retries) {
        if (!registry_) return false;

        RpcTransportFactory factory;
        {
            std::lock_guard<std::mutex> lock(mu_);
            factory = factory_;
        }
        if (!factory) return false;

        NodeInfo primary;
        if (!registry_->select_node_by_hash(route_key, primary, service_type)) {
            return false;
        }

        // 主选节点 + 按同类型节点列表 failover（确定性，不依赖哈希再采样）
        std::vector<NodeInfo> candidates;
        candidates.push_back(primary);
        for (const auto& n : registry_->find_nodes_by_type(service_type)) {
            if (n.node_id != primary.node_id) {
                candidates.push_back(n);
            }
        }

        int max_attempts = static_cast<int>(std::min<std::size_t>(candidates.size(),
            static_cast<std::size_t>(std::max(0, failover_retries)) + 1));

        for (int attempts = 0; attempts < max_attempts; ++attempts) {
            const NodeInfo& node = candidates[attempts];
            RpcTransportPtr tp = get_or_create(factory, node);
            if (!tp) continue;
            std::vector<char> resp;
            if (tp->call(cmd, request, resp, timeout_ms)) {
                response = std::move(resp);
                return true;
            }
            // 连接层坏了：丢弃缓存，换下一个节点
            drop(node.node_id);
        }
        return false;
    }

    // 按显式节点转发（已知 session 所在节点时使用）
    bool forward_to_node(const NodeInfo& node,
                         std::uint16_t cmd,
                         const std::vector<char>& request,
                         std::vector<char>& response,
                         int timeout_ms = 2000) {
        RpcTransportFactory factory;
        {
            std::lock_guard<std::mutex> lock(mu_);
            factory = factory_;
        }
        if (!factory) return false;
        RpcTransportPtr tp = get_or_create(factory, node);
        if (!tp) return false;
        std::vector<char> resp;
        if (!tp->call(cmd, request, resp, timeout_ms)) {
            drop(node.node_id);
            return false;
        }
        response = std::move(resp);
        return true;
    }

    // 指定节点已下线时立刻剔除
    void drop(const std::string& node_id) {
        std::lock_guard<std::mutex> lock(mu_);
        transports_.erase(node_id);
    }

    void set_failover_retries(int n) { failover_retries_ = n < 0 ? 0 : n; }

    std::size_t connection_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return transports_.size();
    }

private:
    RpcTransportPtr get_or_create(const RpcTransportFactory& factory, const NodeInfo& node) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = transports_.find(node.node_id);
            if (it != transports_.end() && it->second.transport && it->second.transport->healthy() &&
                it->second.node.listen_addr == node.listen_addr &&
                it->second.node.listen_port == node.listen_port &&
                it->second.node.incarnation == node.incarnation) return it->second.transport;
        }
        // Connecting can block or re-enter the router; never invoke the factory under mu_.
        RpcTransportPtr tp = factory(node);
        if (!tp) return nullptr;
        std::lock_guard<std::mutex> lock(mu_);
        transports_[node.node_id] = {node, tp};
        return tp;
    }

    std::shared_ptr<NodeRegistry> registry_;
    mutable std::mutex mu_;
    RpcTransportFactory factory_;
    struct CachedTransport { NodeInfo node; RpcTransportPtr transport; };
    std::unordered_map<std::string, CachedTransport> transports_;
    std::atomic<int> failover_retries_{1};
};

}  // namespace cluster
}  // namespace chwell
