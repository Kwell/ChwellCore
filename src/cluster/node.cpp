#include "chwell/cluster/node.h"
#include "chwell/core/logger.h"
#include "chwell/discovery/service_discovery.h"

namespace chwell {
namespace cluster {

Node::Node(net::IoService& io_service,
           const std::string& node_id,
           const std::string& node_type,
           const std::string& listen_addr,
           unsigned short listen_port,
           int heartbeat_interval_seconds)
    : io_service_(io_service)
    , node_id_(node_id)
    , node_type_(node_type)
    , listen_addr_(listen_addr)
    , listen_port_(listen_port)
    , heartbeat_interval_seconds_(heartbeat_interval_seconds) {
}

Node::~Node() {
    stop();
}

bool Node::start(discovery::ServiceDiscovery* discovery) {
    if (running_.load()) {
        CHWELL_LOG_WARN("Node " + node_id_ + " already running");
        return false;
    }
    if (!discovery) {
        CHWELL_LOG_ERROR("Node " + node_id_ + ": discovery is null");
        return false;
    }

    discovery_ = discovery;

    // Register with discovery
    discovery::ServiceInstance instance;
    instance.service_id   = node_type_;
    instance.instance_id  = node_id_;
    instance.host         = listen_addr_;
    instance.port         = listen_port_;
    instance.is_alive     = true;
    instance.metadata["node_type"] = node_type_;

    if (!discovery_->register_service(instance)) {
        CHWELL_LOG_ERROR("Node " + node_id_ + ": failed to register with discovery");
        return false;
    }

    running_.store(true);
    heartbeat_thread_ = std::thread([this]() { heartbeat_loop(); });

    CHWELL_LOG_INFO("Node " + node_id_ + " (type=" + node_type_ + ") started, heartbeat_interval="
                    + std::to_string(heartbeat_interval_seconds_) + "s");
    return true;
}

void Node::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    hb_cv_.notify_all();
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    if (discovery_) {
        discovery_->deregister_service(node_id_);
        CHWELL_LOG_INFO("Node " + node_id_ + " deregistered from discovery");
        discovery_ = nullptr;
    }

    CHWELL_LOG_INFO("Node " + node_id_ + " stopped");
}

void Node::heartbeat_loop() {
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(hb_mutex_);
        hb_cv_.wait_for(lock,
                        std::chrono::seconds(heartbeat_interval_seconds_),
                        [this]() { return !running_.load(); });

        if (!running_.load()) break;

        if (discovery_) {
            if (!discovery_->heartbeat(node_id_)) {
                CHWELL_LOG_WARN("Node " + node_id_ + ": heartbeat failed (instance may have expired)");
                // Re-register if heartbeat fails (e.g., discovery was restarted)
                discovery::ServiceInstance instance;
                instance.service_id   = node_type_;
                instance.instance_id  = node_id_;
                instance.host         = listen_addr_;
                instance.port         = listen_port_;
                instance.is_alive     = true;
                instance.metadata["node_type"] = node_type_;
                discovery_->register_service(instance);
            }
        }
    }
}

} // namespace cluster
} // namespace chwell
