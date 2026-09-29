#pragma once

// 运行时指标埋点：QPS / 延迟分布 / 连接数 / 线程池
//
// 与 PrometheusRegistry 的关系：
// - PrometheusRegistry 负责指标的注册与文本导出（已有）
// - Instrumentation 负责在业务路径上打点（新增）
//
// 用法：
//   auto& inst = metrics::Instrumentation::instance();
//   inst.record_request("login", elapsed_ms);   // 自动维护 QPS + 直方图
//   inst.on_connect(); / inst.on_disconnect();
//   std::string text = inst.render_prometheus(); // 拼接 Prometheus 文本

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <sstream>

namespace chwell {
namespace metrics {

// 固定桶延迟直方图（毫秒）
class LatencyHistogram {
public:
    LatencyHistogram() : buckets_{1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000}, counts_(buckets_.size() + 1, 0) {}

    void observe(double ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++total_;
        sum_ms_ += ms;
        if (ms < min_ms_ || total_ == 1) min_ms_ = ms;
        if (ms > max_ms_) max_ms_ = ms;
        std::size_t i = 0;
        for (; i < buckets_.size(); ++i) {
            if (ms <= buckets_[i]) break;
        }
        ++counts_[i];
    }

    void render(std::ostringstream& oss, const std::string& name, const std::string& help) const {
        std::lock_guard<std::mutex> lock(mutex_);
        oss << "# HELP " << name << " " << help << "\n";
        oss << "# TYPE " << name << " histogram\n";
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < buckets_.size(); ++i) {
            cum += counts_[i];
            oss << name << "_bucket{le=\"" << buckets_[i] << "\"} " << cum << "\n";
        }
        cum += counts_.back();
        oss << name << "_bucket{le=\"+Inf\"} " << cum << "\n";
        oss << name << "_sum " << sum_ms_ << "\n";
        oss << name << "_count " << total_ << "\n";
        oss << name << "_min_ms " << min_ms_ << "\n";
        oss << name << "_max_ms " << max_ms_ << "\n";
    }

    std::uint64_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_;
    }

    double avg_ms() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_ ? sum_ms_ / static_cast<double>(total_) : 0.0;
    }

private:
    mutable std::mutex mutex_;
    std::vector<double> buckets_;
    std::vector<std::uint64_t> counts_;
    std::uint64_t total_ = 0;
    double sum_ms_ = 0;
    double min_ms_ = 0;
    double max_ms_ = 0;
};

class Instrumentation {
public:
    static Instrumentation& instance() {
        static Instrumentation inst;
        return inst;
    }

    // ---- 连接 ----
    void on_connect() {
        conns_total_.fetch_add(1, std::memory_order_relaxed);
        conns_current_.fetch_add(1, std::memory_order_relaxed);
    }
    void on_disconnect() {
        conns_current_.fetch_sub(1, std::memory_order_relaxed);
    }

    // ---- 请求 ----
    void record_request(const std::string& name, double elapsed_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& h = histograms_[name];
        h.observe(elapsed_ms);
        ++requests_total_;
    }

    // ---- 错误 ----
    void record_error(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++errors_[name];
        ++errors_total_;
    }

    // ---- 逻辑线程积压 ----
    void set_logic_pending(std::uint64_t n) {
        logic_pending_.store(n, std::memory_order_relaxed);
    }

    // ---- Prometheus 文本 ----
    std::string render_prometheus() const {
        std::ostringstream oss;
        oss << "# HELP chwell_connections_total Accepted connections\n";
        oss << "# TYPE chwell_connections_total counter\n";
        oss << "chwell_connections_total " << conns_total_.load() << "\n";
        oss << "# HELP chwell_connections_current Current connections\n";
        oss << "# TYPE chwell_connections_current gauge\n";
        oss << "chwell_connections_current " << conns_current_.load() << "\n";
        oss << "# HELP chwell_requests_total Total business requests\n";
        oss << "# TYPE chwell_requests_total counter\n";
        oss << "chwell_requests_total " << requests_total_ << "\n";
        oss << "# HELP chwell_errors_total Total business errors\n";
        oss << "# TYPE chwell_errors_total counter\n";
        oss << "chwell_errors_total " << errors_total_ << "\n";
        oss << "# HELP chwell_logic_pending Logic queue backlog\n";
        oss << "# TYPE chwell_logic_pending gauge\n";
        oss << "chwell_logic_pending " << logic_pending_.load() << "\n";

        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kv : histograms_) {
            kv.second.render(oss, "chwell_request_duration_ms{name=\"" + kv.first + "\"}",
                             "Request duration in ms");
        }
        for (const auto& kv : errors_) {
            oss << "# HELP chwell_error_total{name=\"" << kv.first << "\"} errors\n";
            oss << "# TYPE chwell_error_total{name=\"" << kv.first << "\"} counter\n";
            oss << "chwell_error_total{name=\"" << kv.first << "\"} " << kv.second << "\n";
        }
        return oss.str();
    }

    // ---- 运维快照（供 OpsComponent 输出）----
    std::string render_status_json() const {
        std::ostringstream oss;
        oss << "{\"connections_current\":" << conns_current_.load()
            << ",\"connections_total\":" << conns_total_.load()
            << ",\"requests_total\":" << requests_total_
            << ",\"errors_total\":" << errors_total_
            << ",\"logic_pending\":" << logic_pending_.load()
            << ",\"handlers\":[";
        bool first = true;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kv : histograms_) {
            if (!first) oss << ",";
            first = false;
            oss << "{\"name\":\"" << kv.first
                << "\",\"count\":" << kv.second.count()
                << ",\"avg_ms\":" << kv.second.avg_ms() << "}";
        }
        oss << "]}";
        return oss.str();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        conns_total_.store(0);
        conns_current_.store(0);
        requests_total_ = 0;
        errors_total_ = 0;
        histograms_.clear();
        errors_.clear();
    }

private:
    std::atomic<std::uint64_t> conns_total_{0};
    std::atomic<std::uint64_t> conns_current_{0};
    std::atomic<std::uint64_t> logic_pending_{0};
    mutable std::mutex mutex_;
    std::unordered_map<std::string, LatencyHistogram> histograms_;
    std::unordered_map<std::string, std::uint64_t> errors_;
    std::uint64_t requests_total_ = 0;
    std::uint64_t errors_total_ = 0;
};

} // namespace metrics
} // namespace chwell
