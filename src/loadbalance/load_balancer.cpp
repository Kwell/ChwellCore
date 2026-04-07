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
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        // 如果实例列表已缓存，直接轮询（快速路径）
        if (!instances_.empty()) {
            size_t index = current_index_.fetch_add(1, std::memory_order_relaxed) % instances_.size();
            out = instances_[index];
            return true;
        }
    }

    // 慢速路径：需要从服务发现获取实例（需要写锁）
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        // 双重检查，可能其他线程已经填充了
        if (!instances_.empty()) {
            size_t index = current_index_.fetch_add(1, std::memory_order_relaxed) % instances_.size();
            out = instances_[index];
            return true;
        }

        CHWELL_LOG_DEBUG("Fetching instances for service: " << service_id);
        instances_ = discovery_->discover_services(service_id);
        if (instances_.empty()) {
            CHWELL_LOG_WARN("No available instances for service: " + service_id);
            return false;
        }

        size_t index = current_index_.fetch_add(1, std::memory_order_relaxed) % instances_.size();
        out = instances_[index];

        CHWELL_LOG_DEBUG("Fetched " << instances_.size() << " instances for service: " << service_id);
        return true;
    }
}

void RoundRobinLoadBalancer::update_instances(const std::vector<discovery::ServiceInstance>& instances) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    instances_ = instances;
    CHWELL_LOG_INFO("Updated " + std::to_string(instances.size()) +
                    " instances for RoundRobinLoadBalancer");
}

// ============================================
// RandomLoadBalancer
// ============================================

bool RandomLoadBalancer::select_instance(const std::string& service_id, discovery::ServiceInstance& out) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    if (instances_.empty()) {
        instances_ = discovery_->discover_services(service_id);
        if (instances_.empty()) {
            CHWELL_LOG_WARN("No available instances for service: " + service_id);
            return false;
        }
    }

    std::uniform_int_distribution<size_t> dist(0, instances_.size() - 1);
    size_t index = dist(rng_);
    out = instances_[index];
    return true;
}

void RandomLoadBalancer::update_instances(const std::vector<discovery::ServiceInstance>& instances) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    instances_ = instances;
    CHWELL_LOG_INFO("Updated " + std::to_string(instances.size()) +
                    " instances for RandomLoadBalancer");
}

// ============================================
// WeightedRoundRobinLoadBalancer（优化版）
// ============================================

void WeightedRoundRobinLoadBalancer::rebuild_cache() {
    cache_.clear();
    cache_.reserve(instances_.size());
    total_weight_ = 0;

    for (const auto& inst : instances_) {
        auto it = weights_.find(inst.instance_id);
        int w = (it != weights_.end()) ? it->second : 1;
        cache_.push_back({inst, w, 0});
        total_weight_ += w;
    }

    dirty_ = false;
}

bool WeightedRoundRobinLoadBalancer::select_instance(const std::string& service_id, discovery::ServiceInstance& out) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

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

        dirty_ = true;
    }

    // 如果需要重建缓存
    if (dirty_) {
        rebuild_cache();
    }

    // 单实例快速路径
    if (cache_.size() == 1) {
        out = cache_[0].instance;
        return true;
    }

    if (total_weight_ <= 0) {
        CHWELL_LOG_ERROR("Invalid total weight: " + std::to_string(total_weight_));
        return false;
    }

    // 平滑加权轮询算法（遍历紧凑数组，无 map 查找）
    size_t best_index = 0;
    int max_current_weight = std::numeric_limits<int>::min();

    for (size_t i = 0; i < cache_.size(); ++i) {
        cache_[i].current_weight += cache_[i].weight;

        if (cache_[i].current_weight > max_current_weight) {
            max_current_weight = cache_[i].current_weight;
            best_index = i;
        }
    }

    // 被选中的实例减去总权重
    cache_[best_index].current_weight -= total_weight_;

    out = cache_[best_index].instance;
    return true;
}

void WeightedRoundRobinLoadBalancer::update_instances(const std::vector<discovery::ServiceInstance>& instances) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    instances_ = instances;
    dirty_ = true;
    CHWELL_LOG_INFO("Updated " + std::to_string(instances.size()) +
                    " instances for WeightedRoundRobinLoadBalancer");
}

} // namespace loadbalance
} // namespace chwell
