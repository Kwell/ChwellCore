#pragma once

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "chwell/ops/analytics.h"
#include "chwell/storage/storage_interface.h"
#include "chwell/storage/orm/document.h"

namespace chwell {
namespace ops {

// ========== 分析数据持久化 ==========
//
// AnalyticsStore：把 AnalyticsPipeline 的聚合结果写入 StorageInterface，
// 便于长期保存与跨进程查询。不替代时序库，只做轻量快照。
//
// 键布局：
//   analytics:count:{event}:{from}-{to}     → 次数
//   analytics:uv:{event}:{from}-{to}        → 去重用户数
//   analytics:snapshot:{yyyymmdd}           → 当日 top 事件 JSON 文本
//
// - flush_counts：把指定区间 count/uv 落盘
// - flush_top：把 top 事件落盘
// - load_count / load_uv：读回

class AnalyticsStore {
public:
    AnalyticsStore(AnalyticsPipeline* pipeline, storage::StorageInterface* storage)
        : pipeline_(pipeline), storage_(storage) {}

    bool flush_counts(const std::string& event, std::int64_t from_ts, std::int64_t to_ts) {
        if (!pipeline_ || !storage_ || event.empty()) return false;
        std::string suffix = event + ":" + std::to_string(from_ts) + "-" + std::to_string(to_ts);
        auto c = pipeline_->count(event, from_ts, to_ts);
        auto u = pipeline_->unique_users(event, from_ts, to_ts);
        auto r1 = storage_->put("analytics:count:" + suffix, std::to_string(c));
        auto r2 = storage_->put("analytics:uv:" + suffix, std::to_string(u));
        return r1.ok && r2.ok;
    }

    bool flush_top(const std::string& snapshot_id, std::size_t n = 10) {
        if (!pipeline_ || !storage_ || snapshot_id.empty()) return false;
        auto top = pipeline_->top_events(n);
        std::ostringstream os;
        os << "{\"snapshot\":\"" << snapshot_id << "\",\"events\":[";
        for (std::size_t i = 0; i < top.size(); ++i) {
            if (i) os << ",";
            os << "{\"name\":\"" << top[i].first << "\",\"count\":" << top[i].second << "}";
        }
        os << "]}";
        auto r = storage_->put("analytics:snapshot:" + snapshot_id, os.str());
        return r.ok;
    }

    bool load_count(const std::string& event, std::int64_t from_ts, std::int64_t to_ts,
                    std::uint64_t& out) const {
        return load_u64("analytics:count:" + event + ":" + std::to_string(from_ts) + "-"
                                        + std::to_string(to_ts), out);
    }

    bool load_uv(const std::string& event, std::int64_t from_ts, std::int64_t to_ts,
                 std::uint64_t& out) const {
        return load_u64("analytics:uv:" + event + ":" + std::to_string(from_ts) + "-"
                                    + std::to_string(to_ts), out);
    }

    bool load_top(const std::string& snapshot_id, std::string& json_out) const {
        if (!storage_) return false;
        auto r = storage_->get("analytics:snapshot:" + snapshot_id);
        if (!r.ok) return false;
        json_out = r.value;
        return true;
    }

private:
    bool load_u64(const std::string& key, std::uint64_t& out) const {
        if (!storage_) return false;
        auto r = storage_->get(key);
        if (!r.ok) return false;
        try {
            out = static_cast<std::uint64_t>(std::stoull(r.value));
            return true;
        } catch (...) {
            return false;
        }
    }

    AnalyticsPipeline* pipeline_;
    storage::StorageInterface* storage_;
};

}  // namespace ops
}  // namespace chwell
