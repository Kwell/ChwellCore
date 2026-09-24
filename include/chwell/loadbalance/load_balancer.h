#pragma once

#include "chwell/core/logger.h"
#include "chwell/discovery/service_discovery.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <random>
#include <atomic>
#include <shared_mutex>

namespace chwell {
namespace loadbalance {

// 负载均衡策略
enum class LoadBalanceStrategy {
    ROUND_ROBIN,        // 轮询
    RANDOM,             // 随机
    LEAST_CONNECTIONS, // 最小连接
    WEIGHTED_ROUND_ROBIN, // 加权轮询
    WEIGHTED_RANDOM,    // 加权随机
};

// 负载均衡器接口
class LoadBalancer {
public:
    virtual ~LoadBalancer() = default;

    // 选择一个服务实例
    virtual bool select_instance(const std::string& service_id, discovery::ServiceInstance& out) = 0;

    // 设置负载均衡策略
    virtual void set_strategy(LoadBalanceStrategy strategy) = 0;

    // 获取当前策略
    virtual LoadBalanceStrategy get_strategy() const = 0;

    // 更新指定服务的实例列表
    virtual void update_instances(const std::string& service_id,
                                  const std::vector<discovery::ServiceInstance>& instances) = 0;

    // 设置实例权重（用于加权策略）
    virtual void set_weight(const std::string& instance_id, int weight) = 0;

    // 获取实例权重
    virtual int get_weight(const std::string& instance_id) const = 0;
};

// ============================================
// 轮询负载均衡器
// ============================================

class RoundRobinLoadBalancer : public LoadBalancer {
public:
    RoundRobinLoadBalancer(std::shared_ptr<discovery::ServiceDiscovery> discovery)
        : discovery_(discovery), current_index_(0) {}

    virtual ~RoundRobinLoadBalancer() = default;

    virtual bool select_instance(const std::string& service_id, discovery::ServiceInstance& out) override;

    virtual void set_strategy(LoadBalanceStrategy strategy) override {
        strategy_ = strategy;
    }

    virtual LoadBalanceStrategy get_strategy() const override {
        return strategy_;
    }

    virtual void update_instances(const std::string& service_id,
                                  const std::vector<discovery::ServiceInstance>& instances) override;

    virtual void set_weight(const std::string& instance_id, int weight) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        weights_[instance_id] = weight;
    }

    virtual int get_weight(const std::string& instance_id) const override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = weights_.find(instance_id);
        if (it != weights_.end()) {
            return it->second;
        }
        return 1; // 默认权重
    }

private:
    std::shared_ptr<discovery::ServiceDiscovery> discovery_;
    LoadBalanceStrategy strategy_ = LoadBalanceStrategy::ROUND_ROBIN;
    std::atomic<size_t> current_index_;
    std::unordered_map<std::string, std::vector<discovery::ServiceInstance>> instances_by_service_;
    std::unordered_map<std::string, int> weights_;
    mutable std::shared_mutex mutex_;
};

// ============================================
// 随机负载均衡器
// ============================================

class RandomLoadBalancer : public LoadBalancer {
public:
    RandomLoadBalancer(std::shared_ptr<discovery::ServiceDiscovery> discovery)
        : discovery_(discovery), rng_(std::random_device{}()) {}

    virtual ~RandomLoadBalancer() = default;

    virtual bool select_instance(const std::string& service_id, discovery::ServiceInstance& out) override;

    virtual void set_strategy(LoadBalanceStrategy strategy) override {
        strategy_ = strategy;
    }

    virtual LoadBalanceStrategy get_strategy() const override {
        return strategy_;
    }

    virtual void update_instances(const std::string& service_id,
                                  const std::vector<discovery::ServiceInstance>& instances) override;

    virtual void set_weight(const std::string& instance_id, int weight) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        weights_[instance_id] = weight;
    }

    virtual int get_weight(const std::string& instance_id) const override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = weights_.find(instance_id);
        if (it != weights_.end()) {
            return it->second;
        }
        return 1; // 默认权重
    }

private:
    std::shared_ptr<discovery::ServiceDiscovery> discovery_;
    LoadBalanceStrategy strategy_ = LoadBalanceStrategy::RANDOM;
    std::mt19937 rng_;
    std::unordered_map<std::string, std::vector<discovery::ServiceInstance>> instances_by_service_;
    std::unordered_map<std::string, int> weights_;
    mutable std::shared_mutex mutex_;
};

// ============================================
// 加权轮询负载均衡器（优化版）
// 使用结构体数组替代 unordered_map，缓存 total_weight，
// select_instance 遍历连续内存数组，避免 map 查找开销。
// ============================================

class WeightedRoundRobinLoadBalancer : public LoadBalancer {
public:
    WeightedRoundRobinLoadBalancer(std::shared_ptr<discovery::ServiceDiscovery> discovery)
        : discovery_(discovery) {}

    virtual ~WeightedRoundRobinLoadBalancer() = default;

    virtual bool select_instance(const std::string& service_id, discovery::ServiceInstance& out) override;

    virtual void set_strategy(LoadBalanceStrategy strategy) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        strategy_ = strategy;
    }

    virtual LoadBalanceStrategy get_strategy() const override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return strategy_;
    }

    virtual void update_instances(const std::string& service_id,
                                  const std::vector<discovery::ServiceInstance>& instances) override;

    virtual void set_weight(const std::string& instance_id, int weight) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        weights_[instance_id] = weight;
        for (auto& kv : caches_) {
            kv.second.dirty = true;
        }
    }

    virtual int get_weight(const std::string& instance_id) const override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = weights_.find(instance_id);
        if (it != weights_.end()) {
            return it->second;
        }
        return 1; // 默认权重
    }

private:
    // 重建指定服务的内部缓存数组（写锁已持有时调用）
    void rebuild_cache(const std::string& service_id);

    // 加权实例条目，紧凑排列在连续内存中
    struct WeightedEntry {
        discovery::ServiceInstance instance;
        int weight;          // 配置的权重
        int current_weight;  // 平滑加权轮询的当前权重
    };

    // 每个 service_id 一份紧凑缓存
    struct ServiceCache {
        std::vector<WeightedEntry> cache;
        int total_weight = 0;
        bool dirty = true;
    };

    std::shared_ptr<discovery::ServiceDiscovery> discovery_;
    LoadBalanceStrategy strategy_ = LoadBalanceStrategy::WEIGHTED_ROUND_ROBIN;

    // 配置层（写操作）
    std::unordered_map<std::string, std::vector<discovery::ServiceInstance>> instances_by_service_;
    std::unordered_map<std::string, int> weights_;

    // 紧凑缓存层（读操作热点路径，按 service_id 分键）
    std::unordered_map<std::string, ServiceCache> caches_;

    mutable std::shared_mutex mutex_;
};

} // namespace loadbalance
} // namespace chwell
