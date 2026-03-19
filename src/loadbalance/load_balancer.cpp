#include "chwell/loadbalance/load_balancer.h"
#include <algorithm>
#include <numeric>
#include <limits>

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
        // 按 instance_id 排序，保证顺序稳定
        std::stable_sort(instances_.begin(), instances_.end(),
            [](const discovery::ServiceInstance& a, const discovery::ServiceInstance& b) {
                return a.instance_id < b.instance_id;
            });
    }

    if (instances_.size() == 1) {
        out = instances_[0];
        return true;
    }

    // 计算总权重
    int total_weight = 0;
    for (const auto& instance : instances_) {
        auto it = weights_.find(instance.instance_id);
        int weight = (it != weights_.end()) ? it->second : 1;
        total_weight += weight;

        // 初始化当前权重
        if (current_weights_.find(instance.instance_id) == current_weights_.end()) {
            current_weights_[instance.instance_id] = 0;
        }
    }

    if (total_weight <= 0) {
        CHWELL_LOG_ERROR("Invalid total weight: " + std::to_string(total_weight));
        return false;
    }

    // 平滑加权轮询算法
    // 选择 current_weight 最大的实例
    size_t best_index = 0;
    int max_current_weight = std::numeric_limits<int>::min();

    for (size_t i = 0; i < instances_.size(); ++i) {
        const std::string& instance_id = instances_[i].instance_id;

        // 获取权重
        auto weight_it = weights_.find(instance_id);
        int effective_weight = (weight_it != weights_.end()) ? weight_it->second : 1;

        // 获取当前权重
        int& current_weight = current_weights_[instance_id];
        current_weight += effective_weight;

        // 更新最大值
        if (current_weight > max_current_weight) {
            max_current_weight = current_weight;
            best_index = i;
        }
    }

    // 减去总权重
    current_weights_[instances_[best_index].instance_id] -= total_weight;

    out = instances_[best_index];

    return true;
}

void WeightedRoundRobinLoadBalancer::update_instances(const std::vector<discovery::ServiceInstance>& instances) {
    std::lock_guard<std::mutex> lock(mutex_);

    instances_ = instances;
    current_weights_.clear(); // 清空当前权重

    CHWELL_LOG_INFO("Updated " + std::to_string(instances.size()) +
                    " instances for WeightedRoundRobinLoadBalancer");
}

} // namespace loadbalance
} // namespace chwell
