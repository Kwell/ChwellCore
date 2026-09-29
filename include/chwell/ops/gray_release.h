#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chwell {
namespace ops {

// ========== 灰度发布 / 流量切分 ==========
//
// TrafficSplitter：按权重把请求分到若干版本（如 stable / canary）。
// - add_version(version, weight)：权重越大流量占比越高，0 表示不接流
// - select(key)：一致性哈希风格——同一 key 稳定落同一版本（会话粘滞）
// - select_random()：纯随机按权重抽取（无粘滞）
// - set_weight / remove_version / versions()
//
// 典型用法：网关按 uid 选版本，再把请求打到对应发布组。

class TrafficSplitter {
public:
    struct VersionShare {
        std::string version;
        int weight = 0;
        double share = 0.0;  // 归一化占比
    };

    void add_version(const std::string& version, int weight) {
        if (version.empty() || weight < 0) return;
        std::lock_guard<std::mutex> lock(mu_);
        weights_[version] = weight;
        rebuild_locked();
    }

    void set_weight(const std::string& version, int weight) {
        if (weight < 0) return;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = weights_.find(version);
        if (it == weights_.end()) return;
        it->second = weight;
        rebuild_locked();
    }

    bool remove_version(const std::string& version) {
        std::lock_guard<std::mutex> lock(mu_);
        bool removed = weights_.erase(version) > 0;
        if (removed) rebuild_locked();
        return removed;
    }

    std::vector<VersionShare> versions() const {
        std::lock_guard<std::mutex> lock(mu_);
        return shares_;
    }

    // 同一 key 恒定选同一版本（FNV-1a 哈希 + 累计权重带）
    std::string select(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (shares_.empty()) return std::string();
        if (shares_.size() == 1) return shares_[0].version;

        std::uint64_t h = fnv1a(key);
        double unit = static_cast<double>(h % 1000000ULL) / 1000000.0;  // [0,1)
        double acc = 0.0;
        for (const auto& s : shares_) {
            acc += s.share;
            if (unit < acc) return s.version;
        }
        return shares_.back().version;
    }

    // 随机按权重（注入 rng_val∈[0,1) 便于测试）
    std::string select_random(double rng_val) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (shares_.empty()) return std::string();
        if (rng_val < 0) rng_val = 0;
        if (rng_val >= 1) rng_val = 0.999999;
        double acc = 0.0;
        for (const auto& s : shares_) {
            acc += s.share;
            if (rng_val < acc) return s.version;
        }
        return shares_.back().version;
    }

    int total_weight() const {
        std::lock_guard<std::mutex> lock(mu_);
        int t = 0;
        for (const auto& kv : weights_) t += kv.second;
        return t;
    }

private:
    void rebuild_locked() {
        shares_.clear();
        int total = 0;
        for (const auto& kv : weights_) total += kv.second;
        if (total <= 0) return;
        for (const auto& kv : weights_) {
            if (kv.second <= 0) continue;
            VersionShare s;
            s.version = kv.first;
            s.weight = kv.second;
            s.share = static_cast<double>(kv.second) / total;
            shares_.push_back(s);
        }
        // 稳定顺序：按版本名，保证 select(key) 可复现
        std::sort(shares_.begin(), shares_.end(),
                  [](const VersionShare& a, const VersionShare& b) {
                      return a.version < b.version;
                  });
    }

    static std::uint64_t fnv1a(const std::string& s) {
        std::uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, int> weights_;
    std::vector<VersionShare> shares_;
};

// ========== 配置版本分发 ==========
//
// ConfigVersionStore：为灰度保留多版本配置快照，可切换当前生效版本。
// 与 core::Config 的 snapshot/rollback 互补：这里管「多版本并存 + 原子切换」。

class ConfigVersionStore {
public:
    void put_version(const std::string& version,
                     std::unordered_map<std::string, std::string> kv) {
        if (version.empty()) return;
        std::lock_guard<std::mutex> lock(mu_);
        versions_[version] = std::move(kv);
        if (active_.empty()) active_ = version;
    }

    // 切换生效版本；版本不存在返回 false
    bool activate(const std::string& version) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!versions_.count(version)) return false;
        active_ = version;
        return true;
    }

    std::string active() const {
        std::lock_guard<std::mutex> lock(mu_);
        return active_;
    }

    // 读当前生效配置
    std::string get(const std::string& key,
                    const std::string& default_val = std::string()) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto vi = versions_.find(active_);
        if (vi == versions_.end()) return default_val;
        auto ki = vi->second.find(key);
        return ki == vi->second.end() ? default_val : ki->second;
    }

    bool has_version(const std::string& version) const {
        std::lock_guard<std::mutex> lock(mu_);
        return versions_.count(version) > 0;
    }

    std::size_t version_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return versions_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> versions_;
    std::string active_;
};

}  // namespace ops
}  // namespace chwell
