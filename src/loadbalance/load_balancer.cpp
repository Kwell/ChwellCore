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
        auto it = instances_by_service_.find(service_id);
        if (it != instances_by_service_.end() && !it->second.empty()) {
            size_t index = current_index_.fetch_add(1, std::memory_order_relaxed) % it->second.size();
            out = it->second[index];
            return true;
        }
    }

    // 慢速路径：需要从服务发现获取实例（需要写锁）
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        // 双重检查，可能其他线程已经填充了
        auto it = instances_by_service_.find(service_id);
        if (it != instances_by_service_.end() && !it->second.empty()) {
            size_t index = current_index_.fetch_add(1, std::memory_order_relaxed) % it->second.size();
            out = it->second[index];
            return true;
        }

        CHWELL_LOG_DEBUG("Fetching instances for service: " << service_id);
        auto& list = instances_by_service_[service_id];
        list = discovery_->discover_services(service_id);
        if (list.empty()) {
            CHWELL_LOG_WARN("No available instances for service: " + service_id);
            return false;
        }

        size_t index = current_index_.fetch_add(1, std::memory_order_relaxed) % list.size();
        out = list[index];

        CHWELL_LOG_DEBUG("Fetched " << list.size() << " instances for service: " << service_id);
        return true;
    }
}

void RoundRobinLoadBalancer::update_instances(const std::string& service_id,
                                              const std::vector<discovery::ServiceInstance>& instances) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    instances_by_service_[service_id] = instances;
    CHWELL_LOG_INFO("Updated " + std::to_string(instances.size()) +
                    " instances for RoundRobinLoadBalancer service=" + service_id);
}

// ============================================
// RandomLoadBalancer
// ============================================

bool RandomLoadBalancer::select_instance(const std::string& service_id, discovery::ServiceInstance& out) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = instances_by_service_.find(service_id);
    if (it == instances_by_service_.end() || it->second.empty()) {
        auto& list = instances_by_service_[service_id];
        list = discovery_->discover_services(service_id);
        if (list.empty()) {
            CHWELL_LOG_WARN("No available instances for service: " + service_id);
            return false;
        }
        it = instances_by_service_.find(service_id);
    }

    const auto& list = it->second;
    std::uniform_int_distribution<size_t> dist(0, list.size() - 1);
    size_t index = dist(rng_);
    out = list[index];
    return true;
}

void RandomLoadBalancer::update_instances(const std::string& service_id,
                                          const std::vector<discovery::ServiceInstance>& instances) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    instances_by_service_[service_id] = instances;
    CHWELL_LOG_INFO("Updated " + std::to_string(instances.size()) +
                    " instances for RandomLoadBalancer service=" + service_id);
}

// ============================================
// WeightedRoundRobinLoadBalancer（优化版）
// ============================================

void WeightedRoundRobinLoadBalancer::rebuild_cache(const std::string& service_id) {
    auto& entry = caches_[service_id];
    entry.cache.clear();

    auto it = instances_by_service_.find(service_id);
    if (it == instances_by_service_.end()) {
        entry.total_weight = 0;
        entry.dirty = false;
        return;
    }

    entry.cache.reserve(it->second.size());
    entry.total_weight = 0;

    for (const auto& inst : it->second) {
        auto w_it = weights_.find(inst.instance_id);
        int w = (w_it != weights_.end()) ? w_it->second : 1;
        entry.cache.push_back({inst, w, 0});
        entry.total_weight += w;
    }

    entry.dirty = false;
}

bool WeightedRoundRobinLoadBalancer::select_instance(const std::string& service_id, discovery::ServiceInstance& out) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // 如果实例列表为空，从服务发现获取
    auto it = instances_by_service_.find(service_id);
    if (it == instances_by_service_.end() || it->second.empty()) {
        auto& list = instances_by_service_[service_id];
        list = discovery_->discover_services(service_id);
        if (list.empty()) {
            CHWELL_LOG_WARN("No available instances for service: " + service_id);
            return false;
        }

        // 按 instance_id 排序，保证顺序稳定
        std::stable_sort(list.begin(), list.end(),
            [](const discovery::ServiceInstance& a, const discovery::ServiceInstance& b) {
                return a.instance_id < b.instance_id;
            });

        caches_[service_id].dirty = true;
    }

    // 如果需要重建缓存
    if (caches_[service_id].dirty) {
        rebuild_cache(service_id);
    }

    auto& entry = caches_[service_id];

    // 单实例快速路径
    if (entry.cache.size() == 1) {
        out = entry.cache[0].instance;
        return true;
    }

    if (entry.cache.empty()) {
        CHWELL_LOG_WARN("No cached instances for service: " + service_id);
        return false;
    }

    if (entry.total_weight <= 0) {
        CHWELL_LOG_ERROR("Invalid total weight: " + std::to_string(entry.total_weight));
        return false;
    }

    // 平滑加权轮询算法（遍历紧凑数组，无 map 查找）
    size_t best_index = 0;
    int max_current_weight = std::numeric_limits<int>::min();

    for (size_t i = 0; i < entry.cache.size(); ++i) {
        entry.cache[i].current_weight += entry.cache[i].weight;

        if (entry.cache[i].current_weight > max_current_weight) {
            max_current_weight = entry.cache[i].current_weight;
            best_index = i;
        }
    }

    // 被选中的实例减去总权重
    entry.cache[best_index].current_weight -= entry.total_weight;

    out = entry.cache[best_index].instance;
    return true;
}

void WeightedRoundRobinLoadBalancer::update_instances(const std::string& service_id,
                                                      const std::vector<discovery::ServiceInstance>& instances) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    instances_by_service_[service_id] = instances;
    caches_[service_id].dirty = true;
    CHWELL_LOG_INFO("Updated " + std::to_string(instances.size()) +
                    " instances for WeightedRoundRobinLoadBalancer service=" + service_id);
}

} // namespace loadbalance
} // namespace chwell
