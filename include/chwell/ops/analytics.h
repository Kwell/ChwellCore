#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chwell {
namespace ops {

// ========== 运营数据分析管道 ==========
//
// AnalyticsPipeline：接收埋点事件，做窗口聚合与漏斗统计。
// - track(event, uid, ts)：写入事件流
// - count(event, from_ts, to_ts)：区间计数
// - unique_users(event, from_ts, to_ts)：区间去重用户数
// - funnel(steps, from_ts, to_ts)：按顺序完成各步的人数（同用户）
// - top_events(n)：按总量排前 N
//
// 事件保留上限 max_events_，超出丢弃最旧；生产环境可换外部时序库。

struct AnalyticsEvent {
    std::string name;
    std::string user_id;
    std::int64_t ts = 0;
    std::string props;  // 可选 kv 文本
};

class AnalyticsPipeline {
public:
    explicit AnalyticsPipeline(std::size_t max_events = 100000) : max_events_(max_events) {}

    void track(const std::string& name, const std::string& user_id,
               std::int64_t ts, const std::string& props = std::string()) {
        if (name.empty()) return;
        std::lock_guard<std::mutex> lock(mu_);
        AnalyticsEvent e;
        e.name = name;
        e.user_id = user_id;
        e.ts = ts;
        e.props = props;
        events_.push_back(std::move(e));
        if (events_.size() > max_events_) {
            events_.erase(events_.begin(), events_.begin() + (events_.size() - max_events_));
        }
    }

    std::uint64_t count(const std::string& name, std::int64_t from_ts, std::int64_t to_ts) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::uint64_t n = 0;
        for (const auto& e : events_) {
            if (e.name == name && e.ts >= from_ts && e.ts <= to_ts) ++n;
        }
        return n;
    }

    std::size_t unique_users(const std::string& name, std::int64_t from_ts, std::int64_t to_ts) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::unordered_map<std::string, bool> seen;
        for (const auto& e : events_) {
            if (e.name == name && e.ts >= from_ts && e.ts <= to_ts) {
                seen[e.user_id] = true;
            }
        }
        return seen.size();
    }

    // 漏斗：steps 为有序事件名；返回逐步留存人数（同 user 在区间内按时间顺序完成）
    std::vector<std::size_t> funnel(const std::vector<std::string>& steps,
                                    std::int64_t from_ts, std::int64_t to_ts) const {
        std::vector<std::size_t> result(steps.size(), 0);
        if (steps.empty()) return result;

        std::lock_guard<std::mutex> lock(mu_);
        // 每用户：各步最早完成时间
        std::unordered_map<std::string, std::vector<std::int64_t>> first_hit;  // user → step_idx → ts
        for (const auto& e : events_) {
            if (e.ts < from_ts || e.ts > to_ts) continue;
            for (std::size_t i = 0; i < steps.size(); ++i) {
                if (e.name != steps[i]) continue;
                auto& vec = first_hit[e.user_id];
                if (vec.size() < steps.size()) vec.resize(steps.size(), -1);
                if (vec[i] < 0) vec[i] = e.ts;
                break;
            }
        }

        for (const auto& kv : first_hit) {
            const auto& vec = kv.second;
            bool ok = true;
            std::int64_t prev = from_ts - 1;
            for (std::size_t i = 0; i < steps.size(); ++i) {
                if (vec.size() <= i || vec[i] < 0 || vec[i] < prev) {
                    ok = false;
                    break;
                }
                prev = vec[i];
            }
            if (ok) {
                for (std::size_t i = 0; i < steps.size(); ++i) ++result[i];
            }
        }
        return result;
    }

    std::vector<std::pair<std::string, std::uint64_t>> top_events(std::size_t n) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::map<std::string, std::uint64_t> counter;
        for (const auto& e : events_) ++counter[e.name];
        std::vector<std::pair<std::string, std::uint64_t>> all(counter.begin(), counter.end());
        std::sort(all.begin(), all.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second != b.second) return a.second > b.second;
                      return a.first < b.first;
                  });
        if (all.size() > n) all.resize(n);
        return all;
    }

    std::size_t total_events() const {
        std::lock_guard<std::mutex> lock(mu_);
        return events_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        events_.clear();
    }

private:
    std::size_t max_events_;
    mutable std::mutex mu_;
    std::vector<AnalyticsEvent> events_;
};

}  // namespace ops
}  // namespace chwell
