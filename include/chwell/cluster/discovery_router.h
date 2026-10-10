#pragma once

#include "chwell/discovery/service_discovery.h"
#include "chwell/cluster/rpc_router.h"
#include "chwell/cluster/session_locator.h"

namespace chwell { namespace cluster {

// Owner-thread polling adapter. Refresh and routing must be serialized by the
// application. This adapter owns the complete registry group for service_type.
class DiscoveryRouter {
public:
    DiscoveryRouter(discovery::ServiceDiscovery& discovery, std::shared_ptr<NodeRegistry> registry,
                    RpcRouter& router, SessionLocator& sessions, std::string service_type);
    // Fail closed: backend failure clears this group's routes and local sessions.
    bool refresh();
    bool forward_session(const std::string& session_id, std::uint16_t cmd,
                         const std::vector<char>& request, std::vector<char>& response,
                         int timeout_ms = 2000);
    const std::string& last_error() const { return error_; }
private:
    bool apply(const std::vector<discovery::ServiceInstance>& instances);
    discovery::ServiceDiscovery& discovery_;
    std::shared_ptr<NodeRegistry> registry_;
    RpcRouter& router_;
    SessionLocator& sessions_;
    std::string type_;
    std::string error_;
    std::unordered_map<std::string, NodeInfo> known_;
};

} } // namespace chwell::cluster
