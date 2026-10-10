#pragma once

#include "chwell/discovery/service_discovery.h"
#include <memory>

namespace chwell { namespace discovery {

struct ConsulConfig {
    std::string endpoint = "http://127.0.0.1:8500";
    std::string token;
    std::string ca_file;
    int ttl_seconds = 10;
    int deregister_after_seconds = 60;
    int connect_timeout_ms = 1000;
    int request_timeout_ms = 2000;
};

struct ConsulHttpResponse {
    bool transport_ok = false;
    long status = 0;
    std::string body;
};

// The callback must support concurrent calls. Paths are already URL-escaped.
using ConsulHttpTransport = std::function<ConsulHttpResponse(
    const std::string& method, const std::string& path, const std::string& body)>;

// No background watch: discovery queries publish changes on the caller's thread.
// Each instance ID must be unique within the Consul datacenter. Register/heartbeat/
// deregister use the same agent endpoint; they are not distributed ownership CAS.
class ConsulServiceDiscovery : public ServiceDiscovery {
public:
    ConsulServiceDiscovery(ConsulConfig config, ConsulHttpTransport transport);
    bool register_service(const ServiceInstance& instance) override;
    bool deregister_service(const std::string& instance_id) override;
    bool heartbeat(const std::string& instance_id) override;
    std::vector<ServiceInstance> discover_services(const std::string& service_id) override;
    bool discover_services_checked(const std::string& service_id,
                                   std::vector<ServiceInstance>& out) override;
    bool get_service_instance(const std::string& instance_id, ServiceInstance& out) override;
    void add_listener(const std::string& service_id, ServiceListener listener) override;
    void remove_listener(const std::string& service_id) override;
    std::vector<std::string> get_all_services() override;
    std::string last_error() const;

private:
    bool request(const std::string& method, const std::string& path,
                 const std::string& body, std::string& response);
    bool fail(const std::string& error);
    bool service_names(std::vector<std::string>& names);
    ConsulConfig config_;
    ConsulHttpTransport transport_;
    mutable std::mutex mutex_;
    std::string error_;
    std::unordered_map<std::string, std::vector<ServiceListener>> listeners_;
    std::unordered_map<std::string, std::vector<ServiceInstance>> snapshots_;
};

// Available when linking the optional chwell_consul target (libcurl + JSON).
std::shared_ptr<ConsulServiceDiscovery> make_consul_discovery(const ConsulConfig& config);

} } // namespace chwell::discovery
