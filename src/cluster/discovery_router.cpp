#include "chwell/cluster/discovery_router.h"
#include <stdexcept>

namespace chwell { namespace cluster {
namespace {
bool same_endpoint(const NodeInfo& a, const NodeInfo& b) {
    return a.listen_addr == b.listen_addr && a.listen_port == b.listen_port &&
           a.incarnation == b.incarnation && a.node_type == b.node_type;
}
}

DiscoveryRouter::DiscoveryRouter(discovery::ServiceDiscovery& discovery,
    std::shared_ptr<NodeRegistry> registry, RpcRouter& router, SessionLocator& sessions, std::string type)
    : discovery_(discovery), registry_(std::move(registry)), router_(router), sessions_(sessions), type_(std::move(type)) {
    if (!registry_ || type_.empty() || type_ == "__all__") throw std::invalid_argument("Invalid discovery routing group");
}

bool DiscoveryRouter::apply(const std::vector<discovery::ServiceInstance>& instances) {
    std::vector<NodeInfo> nodes;
    std::unordered_map<std::string, NodeInfo> pending;
    for (const auto& instance : instances) {
        if (!instance.is_alive || instance.service_id != type_) return false;
        NodeInfo node;
        node.node_id = instance.instance_id;
        node.node_type = instance.service_id;
        node.listen_addr = instance.host;
        node.listen_port = instance.port;
        node.online = true;
        auto generation = instance.metadata.find("incarnation");
        if (generation != instance.metadata.end()) node.incarnation = generation->second;
        if (!pending.emplace(node.node_id, node).second) return false;
        nodes.push_back(std::move(node));
    }
    if (!registry_->replace_nodes_by_type(type_, nodes)) return false;
    for (const auto& entry : known_) {
        const auto it = pending.find(entry.first);
        if (it == pending.end() || !same_endpoint(entry.second, it->second)) {
            router_.drop(entry.first);
            sessions_.drop_node(entry.first);
        }
    }
    known_.swap(pending);
    return true;
}

bool DiscoveryRouter::refresh() {
    std::vector<discovery::ServiceInstance> instances;
    bool ok = false;
    try { ok = discovery_.discover_services_checked(type_, instances); } catch (...) { }
    if (!ok) {
        apply({});
        error_ = "Discovery unavailable; routes withdrawn";
        return false;
    }
    if (!apply(instances)) {
        apply({});
        error_ = "Invalid discovery snapshot; routes withdrawn";
        return false;
    }
    error_.clear();
    return true;
}

bool DiscoveryRouter::forward_session(const std::string& session_id, std::uint16_t cmd,
    const std::vector<char>& request, std::vector<char>& response, int timeout_ms) {
    SessionLocation location;
    NodeInfo node;
    if (!sessions_.locate(session_id, location) || !registry_->find_node(location.node_id, node) ||
        node.node_type != type_) return false;
    // Stateful sessions never fail over to another game process automatically.
    return router_.forward_to_node(node, cmd, request, response, timeout_ms);
}

} } // namespace chwell::cluster
