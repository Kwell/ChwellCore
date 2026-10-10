#include "chwell/discovery/consul_discovery.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace chwell { namespace discovery {
namespace {
using Json = nlohmann::json;

std::string escape_path(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%'; out += hex[c >> 4]; out += hex[c & 15];
        }
    }
    return out;
}

bool same_instance(const ServiceInstance& a, const ServiceInstance& b) {
    return a.instance_id == b.instance_id && a.host == b.host && a.port == b.port &&
           a.service_id == b.service_id && a.metadata == b.metadata;
}
} // namespace

ConsulServiceDiscovery::ConsulServiceDiscovery(ConsulConfig config, ConsulHttpTransport transport)
    : config_(std::move(config)), transport_(std::move(transport)) {
    if (!transport_ || config_.ttl_seconds <= 0 || config_.deregister_after_seconds < 60 ||
        config_.connect_timeout_ms <= 0 || config_.request_timeout_ms <= 0) {
        throw std::invalid_argument("Invalid Consul transport or lease/timeout configuration");
    }
}

bool ConsulServiceDiscovery::fail(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = error;
    return false;
}

std::string ConsulServiceDiscovery::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool ConsulServiceDiscovery::request(const std::string& method, const std::string& path,
                                     const std::string& body, std::string& response) {
    ConsulHttpResponse result;
    try { result = transport_(method, path, body); }
    catch (...) { return fail("Consul transport threw"); }
    if (!result.transport_ok) return fail("Consul transport unavailable");
    if (result.status != 200) return fail("Consul HTTP status " + std::to_string(result.status));
    response = std::move(result.body);
    std::lock_guard<std::mutex> lock(mutex_);
    error_.clear();
    return true;
}

bool ConsulServiceDiscovery::register_service(const ServiceInstance& instance) {
    if (instance.instance_id.empty() || instance.service_id.empty() || instance.host.empty() || !instance.port) {
        return fail("Consul registration requires identity and endpoint");
    }
    Json body = {{"ID", instance.instance_id}, {"Name", instance.service_id},
                 {"Address", instance.host}, {"Port", instance.port}, {"Meta", instance.metadata},
                 {"Check", {{"CheckID", "service:" + instance.instance_id},
                            {"TTL", std::to_string(config_.ttl_seconds) + "s"}, {"Status", "passing"},
                            {"DeregisterCriticalServiceAfter", std::to_string(config_.deregister_after_seconds) + "s"}}}};
    std::string ignored;
    return request("PUT", "/v1/agent/service/register", body.dump(), ignored);
}

bool ConsulServiceDiscovery::deregister_service(const std::string& id) {
    if (id.empty()) return fail("Empty Consul instance ID");
    std::string ignored;
    return request("PUT", "/v1/agent/service/deregister/" + escape_path(id), "", ignored);
}

bool ConsulServiceDiscovery::heartbeat(const std::string& id) {
    if (id.empty()) return fail("Empty Consul instance ID");
    std::string ignored;
    return request("PUT", "/v1/agent/check/pass/" + escape_path("service:" + id), "", ignored);
}

std::vector<ServiceInstance> ConsulServiceDiscovery::discover_services(const std::string& service_id) {
    std::vector<ServiceInstance> out;
    discover_services_checked(service_id, out);
    return out;
}

bool ConsulServiceDiscovery::discover_services_checked(const std::string& id,
                                                       std::vector<ServiceInstance>& out) {
    if (id.empty()) return fail("Empty Consul service ID");
    std::string response;
    if (!request("GET", "/v1/health/service/" + escape_path(id) + "?passing=true", "", response)) return false;
    std::vector<ServiceInstance> current;
    try {
        const auto data = Json::parse(response);
        if (!data.is_array()) return fail("Invalid Consul health snapshot");
        std::unordered_set<std::string> ids;
        for (const auto& item : data) {
            const auto& service = item.at("Service");
            ServiceInstance instance;
            instance.service_id = service.at("Service").get<std::string>();
            instance.instance_id = service.at("ID").get<std::string>();
            instance.host = service.value("Address", std::string());
            if (instance.host.empty()) instance.host = item.at("Node").at("Address").get<std::string>();
            if (!service.at("Port").is_number_integer()) return fail("Invalid Consul service port type");
            const auto port = service.at("Port").get<std::int64_t>();
            if (instance.service_id != id || instance.instance_id.empty() || instance.host.empty() ||
                port < 1 || port > 65535 || !ids.insert(instance.instance_id).second) {
                return fail("Invalid or duplicate Consul service endpoint");
            }
            instance.port = static_cast<std::uint16_t>(port);
            if (service.contains("Meta") && !service.at("Meta").is_null()) {
                instance.metadata = service.at("Meta").get<std::unordered_map<std::string, std::string>>();
            }
            // Do not trust an inconsistent passing response from an intermediary.
            const auto& checks = item.at("Checks");
            if (!checks.is_array() || checks.empty()) return fail("Missing Consul health checks");
            for (const auto& check : checks) {
                if (check.at("Status").get<std::string>() != "passing") return fail("Non-passing Consul snapshot");
            }
            instance.is_alive = true;
            current.push_back(std::move(instance));
        }
    } catch (...) { return fail("Invalid Consul health JSON"); }
    std::sort(current.begin(), current.end(), [](const auto& a, const auto& b) { return a.instance_id < b.instance_id; });
    std::vector<ServiceInstance> events;
    std::vector<ServiceListener> listeners;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& previous = snapshots_[id];
        for (const auto& old : previous) {
            if (std::none_of(current.begin(), current.end(), [&](const auto& now) { return now.instance_id == old.instance_id; })) {
                events.push_back(old);
                events.back().is_alive = false;
            }
        }
        for (const auto& now : current) {
            if (std::none_of(previous.begin(), previous.end(), [&](const auto& old) { return same_instance(old, now); })) events.push_back(now);
        }
        previous = current;
        auto it = listeners_.find(id);
        if (it != listeners_.end()) listeners = it->second;
    }
    out = std::move(current);
    for (const auto& event : events) {
        for (const auto& listener : listeners) {
            try { listener(id, event); } catch (...) { /* A listener must not invalidate a snapshot. */ }
        }
    }
    return true;
}

bool ConsulServiceDiscovery::service_names(std::vector<std::string>& names) {
    std::string response;
    if (!request("GET", "/v1/catalog/services", "", response)) return false;
    try {
        const auto data = Json::parse(response);
        if (!data.is_object()) return fail("Invalid Consul catalog JSON");
        names.clear();
        for (auto it = data.begin(); it != data.end(); ++it) names.push_back(it.key());
        return true;
    } catch (...) { return fail("Invalid Consul catalog JSON"); }
}

std::vector<std::string> ConsulServiceDiscovery::get_all_services() {
    std::vector<std::string> out;
    service_names(out);
    return out;
}

bool ConsulServiceDiscovery::get_service_instance(const std::string& id, ServiceInstance& out) {
    if (id.empty()) return fail("Empty Consul instance ID");
    std::vector<std::string> services;
    if (!service_names(services)) return false;
    for (const auto& service : services) {
        std::vector<ServiceInstance> instances;
        // The catalog contains the built-in Consul service without a TTL check.
        if (service == "consul") continue;
        if (!discover_services_checked(service, instances)) return false;
        for (const auto& instance : instances) {
            if (instance.instance_id == id) { out = instance; return true; }
        }
    }
    return fail("Consul instance not found");
}

void ConsulServiceDiscovery::add_listener(const std::string& id, ServiceListener listener) {
    if (id.empty() || !listener) return;
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_[id].push_back(std::move(listener));
}

void ConsulServiceDiscovery::remove_listener(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.erase(id);
}

} } // namespace chwell::discovery
