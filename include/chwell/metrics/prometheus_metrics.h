#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <sstream>
#include <algorithm>
#include <memory>

namespace chwell {
namespace metrics {

// ============================================
// Prometheus Metrics 类型
// ============================================

// Counter（只增不减的计数器）
class Counter {
public:
    void inc(double delta = 1.0) {
        // 使用 compare_exchange 实现 CAS 循环，保证原子 RMW
        // 旧实现 value_.store(value_.load() + delta) 是非原子 RMW，多线程下会丢失更新
        double old_val = value_.load(std::memory_order_relaxed);
        double new_val;
        do {
            new_val = old_val + delta;
        } while (!value_.compare_exchange_weak(old_val, new_val,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed));
    }

    // 允许 registry.reset() 将计数器归零（保持引用有效）
    void set(double value) {
        value_.store(value, std::memory_order_relaxed);
    }

    double get() const {
        return value_.load(std::memory_order_relaxed);
    }

    std::string to_prometheus(const std::string& name,
                               const std::string& help = "") const {
        std::ostringstream oss;
        oss << "# HELP " << name << " " << help << "\n";
        oss << "# TYPE " << name << " counter\n";
        oss << name << " " << get() << "\n";
        return oss.str();
    }

private:
    std::atomic<double> value_{0.0};
};

// Gauge（可增可减的仪表）
class Gauge {
public:
    void inc(double delta = 1.0) {
        // CAS 循环保证原子 RMW
        double old_val = value_.load(std::memory_order_relaxed);
        double new_val;
        do {
            new_val = old_val + delta;
        } while (!value_.compare_exchange_weak(old_val, new_val,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed));
    }

    void dec(double delta = 1.0) {
        // CAS 循环保证原子 RMW
        double old_val = value_.load(std::memory_order_relaxed);
        double new_val;
        do {
            new_val = old_val - delta;
        } while (!value_.compare_exchange_weak(old_val, new_val,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed));
    }

    void set(double value) {
        value_.store(value, std::memory_order_relaxed);
    }

    double get() const {
        return value_.load(std::memory_order_relaxed);
    }

    std::string to_prometheus(const std::string& name,
                               const std::string& help = "") const {
        std::ostringstream oss;
        oss << "# HELP " << name << " " << help << "\n";
        oss << "# TYPE " << name << " gauge\n";
        oss << name << " " << get() << "\n";
        return oss.str();
    }

private:
    std::atomic<double> value_{0.0};
};

// Histogram（直方图，统计分布）
class Histogram {
public:
    explicit Histogram(const std::vector<double>& buckets)
        : buckets_(buckets), counts_(buckets.size()) {}

    void observe(double value) {
        // 使用 CAS 循环保证原子 RMW，避免多线程下丢失更新
        for (size_t i = 0; i < counts_.size(); ++i) {
            if (value <= buckets_[i]) {
                uint64_t old = counts_[i].load(std::memory_order_relaxed);
                while (!counts_[i].compare_exchange_weak(old, old + 1,
                                                          std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {}
            }
        }
        // count_ 和 sum_ 也用 CAS
        uint64_t old_count = count_.load(std::memory_order_relaxed);
        while (!count_.compare_exchange_weak(old_count, old_count + 1,
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {}

        double old_sum = sum_.load(std::memory_order_relaxed);
        double new_sum;
        do {
            new_sum = old_sum + value;
        } while (!sum_.compare_exchange_weak(old_sum, new_sum,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed));
    }

    double get_count() const {
        return count_.load(std::memory_order_relaxed);
    }

    double get_sum() const {
        return sum_.load(std::memory_order_relaxed);
    }

    std::vector<uint64_t> get_counts() const {
        std::vector<uint64_t> result(counts_.size());
        for (size_t i = 0; i < counts_.size(); ++i) {
            result[i] = counts_[i].load(std::memory_order_relaxed);
        }
        return result;
    }

    std::string to_prometheus(const std::string& name,
                               const std::string& help = "") const {
        std::ostringstream oss;
        oss << "# HELP " << name << " " << help << "\n";
        oss << "# TYPE " << name << " histogram\n";

        // _sum
        oss << name << "_sum " << get_sum() << "\n";

        // _count
        oss << name << "_count " << get_count() << "\n";

        // _bucket
        auto counts = get_counts();
        for (size_t i = 0; i < buckets_.size(); ++i) {
            oss << name << "_bucket{le=\"" << buckets_[i] << "\"} "
                << counts[i] << "\n";
        }

        // _bucket{le="+Inf"}
        oss << name << "_bucket{le=\"+Inf\"} " << get_count() << "\n";

        return oss.str();
    }

private:
    std::vector<double> buckets_;
    std::vector<std::atomic<uint64_t>> counts_;
    std::atomic<uint64_t> count_{0};
    std::atomic<double> sum_{0.0};
};

// Summary（分位数统计）
class Summary {
public:
    explicit Summary(const std::vector<double>& quantiles)
        : quantiles_(quantiles) {}

    void observe(double value) {
        std::lock_guard<std::mutex> lock(mutex_);

        values_.push_back(value);

        // 保持最近 1000 个样本
        if (values_.size() > 1000) {
            values_.erase(values_.begin());
        }
    }

    void set_quantiles(const std::vector<double>& quantiles) {
        std::lock_guard<std::mutex> lock(mutex_);
        quantiles_ = quantiles;
    }

    std::vector<double> get_quantiles() const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (values_.empty()) {
            return std::vector<double>(quantiles_.size(), 0.0);
        }

        std::vector<double> sorted = values_;
        std::sort(sorted.begin(), sorted.end());

        std::vector<double> result;
        for (double q : quantiles_) {
            size_t index = static_cast<size_t>(q * sorted.size());
            if (index >= sorted.size()) {
                index = sorted.size() - 1;
            }
            result.push_back(sorted[index]);
        }

        return result;
    }

    uint64_t get_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return values_.size();
    }

    double get_sum() const {
        std::lock_guard<std::mutex> lock(mutex_);
        double sum = 0;
        for (double v : values_) {
            sum += v;
        }
        return sum;
    }

    std::string to_prometheus(const std::string& name,
                               const std::string& help = "") const {
        std::ostringstream oss;
        oss << "# HELP " << name << " " << help << "\n";
        oss << "# TYPE " << name << " summary\n";

        oss << name << "_sum " << get_sum() << "\n";
        oss << name << "_count " << get_count() << "\n";

        auto qs = get_quantiles();
        for (size_t i = 0; i < quantiles_.size(); ++i) {
            oss << name << "{quantile=\"" << quantiles_[i] << "\"} "
                << qs[i] << "\n";
        }

        return oss.str();
    }

private:
    std::vector<double> quantiles_;
    std::vector<double> values_;
    mutable std::mutex mutex_;
};

// ============================================
// Prometheus Metrics Registry
// ============================================

class PrometheusRegistry {
public:
    Counter& register_counter(const std::string& name, const std::string& help = "");

    Gauge& register_gauge(const std::string& name, const std::string& help = "");

    Histogram& register_histogram(const std::string& name,
                                    const std::vector<double>& buckets,
                                    const std::string& help = "");

    Summary& register_summary(const std::string& name,
                                  const std::vector<double>& quantiles,
                                  const std::string& help = "");

    std::string export_metrics() const;

    std::string to_prometheus() const;

    // 清空所有注册的指标（主要用于测试）
    void reset();

private:
    struct MetricInfo {
        std::string help;
        std::string type;  // counter, gauge, histogram, summary
    };

    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Counter>> counters_;
    std::map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::map<std::string, std::unique_ptr<Histogram>> histograms_;
    std::map<std::string, std::unique_ptr<Summary>> summaries_;
    std::map<std::string, MetricInfo> metric_infos_;
};

PrometheusRegistry& get_prometheus_registry();

} // namespace metrics
} // namespace chwell
