#pragma once

#include "chwell/core/logger.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>

namespace chwell {
namespace discovery {

// 服务实例信息
struct ServiceInstance {
    std::string service_id;      // 服务 ID
    std::string instance_id;     // 实例 ID
    std::string host;            // 主机地址
    uint16_t port;              // 端口
    std::unordered_map<std::string, std::string> metadata; // 元数据
    std::int64_t register_time; // 注册时间（时间戳）
    std::int64_t heartbeat_time; // 心跳时间（时间戳）
    bool is_alive;              // 是否存活

    ServiceInstance()
        : port(0), register_time(0), heartbeat_time(0), is_alive(false) {}
};

// 服务监听器回调
using ServiceListener = std::function<void(const std::string&, const ServiceInstance&)>;

// ============================================
// 服务发现接口
// ============================================

class ServiceDiscovery {
public:
    virtual ~ServiceDiscovery() = default;

    // 注册服务实例
    virtual bool register_service(const ServiceInstance& instance) = 0;

    // 注销服务实例
    virtual bool deregister_service(const std::string& instance_id) = 0;

    // 更新心跳
    virtual bool heartbeat(const std::string& instance_id) = 0;

    // 查询服务实例列表
    virtual std::vector<ServiceInstance> discover_services(const std::string& service_id) = 0;

    // 查询单个服务实例
    virtual bool get_service_instance(const std::string& instance_id, ServiceInstance& out) = 0;

    // 注册服务变更监听器
    virtual void add_listener(const std::string& service_id, ServiceListener listener) = 0;

    // 移除服务变更监听器
    virtual void remove_listener(const std::string& service_id) = 0;

    // 获取所有服务 ID
    virtual std::vector<std::string> get_all_services() = 0;
};

// ============================================
// 内存服务发现实现
// ============================================

class MemoryServiceDiscovery : public ServiceDiscovery {
public:
    // cleanup_interval_ms: how often the background thread runs cleanup.
    // 0 disables the background thread (caller must invoke cleanup manually).
    explicit MemoryServiceDiscovery(int64_t heartbeat_timeout_ms = 30000,
                                    int64_t cleanup_interval_ms = 10000);

    virtual ~MemoryServiceDiscovery();

    // 注册服务实例
    virtual bool register_service(const ServiceInstance& instance) override;

    // 注销服务实例
    virtual bool deregister_service(const std::string& instance_id) override;

    // 更新心跳
    virtual bool heartbeat(const std::string& instance_id) override;

    // 查询服务实例列表
    virtual std::vector<ServiceInstance> discover_services(const std::string& service_id) override;

    // 查询单个服务实例
    virtual bool get_service_instance(const std::string& instance_id, ServiceInstance& out) override;

    // 注册服务变更监听器
    virtual void add_listener(const std::string& service_id, ServiceListener listener) override;

    // 移除服务变更监听器
    virtual void remove_listener(const std::string& service_id) override;

    // 获取所有服务 ID
    virtual std::vector<std::string> get_all_services() override;

    // 检查服务实例是否存活
    virtual bool is_alive(const std::string& instance_id);

    // 清理过期实例
    virtual void cleanup_expired_instances();

private:
    // 获取当前时间戳（毫秒）
    std::int64_t current_timestamp_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void start_cleanup_thread();
    void stop_cleanup_thread();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ServiceInstance> instances_; // instance_id -> instance
    std::unordered_map<std::string, std::unordered_set<std::string>> service_index_; // service_id -> instance_ids
    std::unordered_map<std::string, std::vector<ServiceListener>> listeners_; // service_id -> listeners
    int64_t heartbeat_timeout_ms_;
    int64_t cleanup_interval_ms_;

    std::thread cleanup_thread_;
    std::atomic<bool> cleanup_running_{false};
    std::mutex cleanup_mutex_;
    std::condition_variable cleanup_cv_;
};

} // namespace discovery
} // namespace chwell
