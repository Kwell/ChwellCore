#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>

#include "chwell/discovery/service_discovery.h"

namespace chwell {
namespace discovery {

// etcd 服务发现
class EtcdServiceDiscovery : public ServiceDiscovery {
public:
    EtcdServiceDiscovery(const std::string& endpoints = "http://127.0.0.1:2379",
                       const std::string& prefix = "/services/");
    ~EtcdServiceDiscovery() override;

    bool register_service(const ServiceInstance& instance) override;

    bool deregister_service(const std::string& instance_id) override;

    bool heartbeat(const std::string& instance_id) override;

    std::vector<ServiceInstance> discover_services(const std::string& service_id) const override;

    bool get_service_instance(const std::string& instance_id,
                            ServiceInstance& out) const override;

    void add_listener(const std::string& service_id,
                    ServiceListener listener) override;

    void remove_listener(const std::string& service_id) override;

    std::vector<std::string> get_all_services() const override;

private:
    std::string build_key(const std::string& service_id,
                        const std::string& instance_id) const;

    std::string build_prefix(const std::string& service_id) const;

    ServiceInstance parse_value(const std::string& value) const;

    std::string endpoints_;
    std::string prefix_;
    std::string registered_service_id_;
    std::string registered_instance_id_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ServiceListener> listeners_;
};

} // namespace discovery
} // namespace chwell
