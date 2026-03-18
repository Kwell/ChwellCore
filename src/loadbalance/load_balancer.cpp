#include "chwell/loadbalance/load_balancer.h"
#include <algorithm>
#include <numeric>

namespace chwell {
namespace loadbalance {

// ============================================
// RoundRobinLoadBalancer
// ============================================

bool RoundRobinLoadBalancer::select_instance(const std::string& service_id, discovery::ServiceInstance& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果实例列表为空，从服务发现获取
    if (instances_.empty()) {
        instances_ = discovery_->discover_services(service_id);
        if (instances_.empty()) {
            CHWELL_LOG_WARN("No available instances for service: " + service_id);
            return false;
        }
    }

    // 轮询选择
    size_t index = current_index_.fetch_add(1, std::memory_order_relaxed) % instances_.size();
    out = instances_[index];

    return true;
}

void RoundRobinLoadBalancer::update_instances(const std::vector<discovery::ServiceInstance>& instances) {
    std::lock_guard<std::mutex> lock(mutex_);

    instances_ = instances;

    CHWELL_LOG_INFO("Updated " + std::to_string(instances.size()) +
                    " instances for RoundRobinLoadBalancer");
}

// ============================================
// RandomLoadBalancer
// ============================================

bool RandomLoadBalancer::select_instance(const std::string& service_id, discovery::ServiceInstance& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果实例列表为空，从服务发现获取
    if (instances_.empty()) {
        instances_ = discovery_->discover_services(service_id);
        if (instances_.empty()) {
            CHWELL_LOG_WARN("No available instances for service: " + service_id);
            return false;
        }
    }

    // 随机选择
    std::uniform_int_distribution<size_t> dist(0, instances_.size() - 1);
    size_t index = dist(rng_);
    out = instances_[index];

    return true;
}

void RandomLoadBalancer::update_instances(const std::vector<discovery::ServiceInstance>& instances) {
    std::lock_guard<std::mutex> lock(mutex_);

    instances_ = instances;

    CHWELL_LOG_INFO("Updated " + std::to_string(instances.size()) +
                    " instances for RandomLoadBalancer");
}

// ============================================
// WeightedRoundRobinLoadBalancer
// ============================================

bool WeightedRoundRobinLoadBalancer::select_instance(const std::string& service_id, discovery::ServiceInstance& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果实例列表为空，从服务发现获取
    if (instances_.empty()) {
        instances_ = discovery_->discover_services(service_id);
        if (instances_.empty()) {
            CHWELL_LOG_WARN("No available instances for service: " + service_id);
            return false;
        }
    }

    if (instances_.size() == 1) {
        out = instances_[0];
        return true;
    }

    // 计算总权重
    int total_weight = 0;
    for (const auto& instance : instances_) {
        int weight = get_weight(instance.instance_id);
        total_weight += weight;
    }

    if (total_weight <= 0) {
        CHWELL_LOG_ERROR("Invalid total weight: " + std::to_string(total_weight));
        return false;
    }

    // 使用平滑加权轮询算法
    int max_weight = 0;
    int best_index = 0;

    for (size_t i = 0; i < instances_.size(); ++i) {
        int weight = get_weight(instances_[static_cast<size_t>(i)].instance_id);
        current_weight_ += weight;

        if (current_weight_ > max_weight) {
            max_weight = current_weight_;
            best_index = static_cast<int>(i);
        }
    }

    current_weight_ -= total_weight;

    out = instances_[static_cast<size_t>(best_index)];

    return true;
}

void WeightedRoundRobinLoadBalancer::update_instances(const std::vector<discovery::ServiceInstance>& instances) {
    std::lock_guard<std::mutex> lock(mutex_);

    instances_ = instances;

    CHWELL_LOG_INFO("Updated " + std::to_string(instances.size()) +
                    " instances for WeightedRoundRobinLoadBalancer");
}

} // namespace loadbalance
} // namespace chwell
