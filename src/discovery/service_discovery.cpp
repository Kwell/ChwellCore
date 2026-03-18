#include "chwell/discovery/service_discovery.h"
#include <algorithm>

namespace chwell {
namespace discovery {

bool MemoryServiceDiscovery::register_service(const ServiceInstance& instance) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string instance_id = instance.instance_id;
    if (instance_id.empty()) {
        CHWELL_LOG_ERROR("Instance ID cannot be empty");
        return false;
    }

    // 检查是否已存在
    if (instances_.find(instance_id) != instances_.end()) {
        CHWELL_LOG_WARN("Instance already exists: " + instance_id);
        return false;
    }

    // 创建实例副本
    ServiceInstance new_instance = instance;
    new_instance.register_time = current_timestamp_ms();
    new_instance.heartbeat_time = new_instance.register_time;
    new_instance.is_alive = true;

    // 添加到实例表
    instances_[instance_id] = new_instance;

    // 添加到服务索引
    service_index_[new_instance.service_id].insert(instance_id);

    CHWELL_LOG_INFO("Service registered: " + new_instance.service_id +
                    ", instance: " + instance_id +
                    ", host: " + new_instance.host +
                    ", port: " + std::to_string(new_instance.port));

    // 通知监听器
    auto it = listeners_.find(new_instance.service_id);
    if (it != listeners_.end()) {
        for (auto& listener : it->second) {
            listener(new_instance.service_id, new_instance);
        }
    }

    return true;
}

bool MemoryServiceDiscovery::deregister_service(const std::string& instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = instances_.find(instance_id);
    if (it == instances_.end()) {
        CHWELL_LOG_WARN("Instance not found: " + instance_id);
        return false;
    }

    ServiceInstance instance = it->second;
    std::string service_id = instance.service_id;

    // 从实例表移除
    instances_.erase(it);

    // 从服务索引移除
    auto sit = service_index_.find(service_id);
    if (sit != service_index_.end()) {
        sit->second.erase(instance_id);
        if (sit->second.empty()) {
            service_index_.erase(sit);
        }
    }

    CHWELL_LOG_INFO("Service deregistered: " + service_id +
                    ", instance: " + instance_id);

    return true;
}

bool MemoryServiceDiscovery::heartbeat(const std::string& instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = instances_.find(instance_id);
    if (it == instances_.end()) {
        CHWELL_LOG_WARN("Instance not found: " + instance_id);
        return false;
    }

    it->second.heartbeat_time = current_timestamp_ms();
    it->second.is_alive = true;

    // CHWELL_LOG_DEBUG("Heartbeat: " + instance_id);

    return true;
}

std::vector<ServiceInstance> MemoryServiceDiscovery::discover_services(const std::string& service_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<ServiceInstance> result;

    auto sit = service_index_.find(service_id);
    if (sit == service_index_.end()) {
        CHWELL_LOG_WARN("Service not found: " + service_id);
        return result;
    }

    for (const std::string& instance_id : sit->second) {
        auto it = instances_.find(instance_id);
        if (it != instances_.end() && it->second.is_alive) {
            result.push_back(it->second);
        }
    }

    CHWELL_LOG_DEBUG("Discovered " + std::to_string(result.size()) +
                     " instances for service: " + service_id);

    return result;
}

bool MemoryServiceDiscovery::get_service_instance(const std::string& instance_id, ServiceInstance& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = instances_.find(instance_id);
    if (it == instances_.end()) {
        CHWELL_LOG_WARN("Instance not found: " + instance_id);
        return false;
    }

    out = it->second;
    return true;
}

void MemoryServiceDiscovery::add_listener(const std::string& service_id, ServiceListener listener) {
    std::lock_guard<std::mutex> lock(mutex_);

    listeners_[service_id].push_back(listener);

    CHWELL_LOG_INFO("Listener added for service: " + service_id);
}

void MemoryServiceDiscovery::remove_listener(const std::string& service_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = listeners_.find(service_id);
    if (it != listeners_.end()) {
        listeners_.erase(it);
        CHWELL_LOG_INFO("Listener removed for service: " + service_id);
    }
}

std::vector<std::string> MemoryServiceDiscovery::get_all_services() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> result;
    for (const auto& pair : service_index_) {
        result.push_back(pair.first);
    }

    return result;
}

bool MemoryServiceDiscovery::is_alive(const std::string& instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = instances_.find(instance_id);
    if (it == instances_.end()) {
        return false;
    }

    return it->second.is_alive;
}

void MemoryServiceDiscovery::cleanup_expired_instances() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::int64_t now = current_timestamp_ms();
    std::vector<std::string> expired_instances;

    // 查找过期实例
    for (auto& pair : instances_) {
        if (!pair.second.is_alive) {
            continue;
        }

        if (now - pair.second.heartbeat_time > heartbeat_timeout_ms_) {
            expired_instances.push_back(pair.first);
        }
    }

    // 清理过期实例
    for (const std::string& instance_id : expired_instances) {
        auto it = instances_.find(instance_id);
        if (it != instances_.end()) {
            ServiceInstance instance = it->second;
            std::string service_id = instance.service_id;

            instances_.erase(it);

            // 从服务索引移除
            auto sit = service_index_.find(service_id);
            if (sit != service_index_.end()) {
                sit->second.erase(instance_id);
                if (sit->second.empty()) {
                    service_index_.erase(sit);
                }
            }

            CHWELL_LOG_WARN("Instance expired: " + instance_id +
                           ", service: " + service_id);
        }
    }

    if (!expired_instances.empty()) {
        CHWELL_LOG_INFO("Cleaned up " + std::to_string(expired_instances.size()) +
                       " expired instances");
    }
}

} // namespace discovery
} // namespace chwell
