#pragma once

#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <functional>

#include "chwell/net/posix_io.h"

namespace chwell {

namespace discovery {
class ServiceDiscovery;
struct ServiceInstance;
}

namespace cluster {

// Node 代表集群中的一个服务节点。
// 职责：注册到 ServiceDiscovery 并定期发送心跳，下线时主动注销。
//
// NodeRegistry 职责：静态节点表（YAML 配置加载、内存查找）。
// ServiceDiscovery 职责：动态成员管理（心跳过期、监听器通知）。
// 两者互补，Node 是将自身信息写入 ServiceDiscovery 的桥梁。

class Node {
public:
    Node(net::IoService& io_service,
         const std::string& node_id,
         const std::string& node_type,
         const std::string& listen_addr,
         unsigned short listen_port,
         int heartbeat_interval_seconds = 10);

    ~Node();

    const std::string& id() const { return node_id_; }
    const std::string& type() const { return node_type_; }

    // 启动心跳：向 discovery 注册并开始定期心跳
    // discovery 生命周期需长于 Node
    bool start(discovery::ServiceDiscovery* discovery);

    // 停止心跳并主动注销
    void stop();

    bool is_running() const { return running_.load(); }

private:
    void heartbeat_loop();

    net::IoService& io_service_;
    std::string node_id_;
    std::string node_type_;
    std::string listen_addr_;
    unsigned short listen_port_;
    int heartbeat_interval_seconds_;

    discovery::ServiceDiscovery* discovery_{nullptr};

    std::atomic<bool> running_{false};
    std::thread heartbeat_thread_;
    std::mutex hb_mutex_;
    std::condition_variable hb_cv_;
};

} // namespace cluster
} // namespace chwell
