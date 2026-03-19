#include "chwell/metrics/prometheus_metrics.h"

#include <algorithm>
#include <memory>

namespace chwell {
namespace metrics {

Counter& PrometheusRegistry::register_counter(
    const std::string& name, const std::string& help) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = counters_.find(name);
    if (it != counters_.end()) {
        return *it->second;  // 返回已存在的 Counter
    }

    MetricInfo info;
    info.help = help;
    info.type = "counter";
    metric_infos_[name] = info;

    // 先创建空 unique_ptr，再赋值
    counters_[name] = std::make_unique<Counter>();
    return *counters_[name];
}

Gauge& PrometheusRegistry::register_gauge(
    const std::string& name, const std::string& help) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = gauges_.find(name);
    if (it != gauges_.end()) {
        return *it->second;  // 返回已存在的 Gauge
    }

    MetricInfo info;
    info.help = help;
    info.type = "gauge";
    metric_infos_[name] = info;

    // 先创建空 unique_ptr，再赋值
    gauges_[name] = std::make_unique<Gauge>();
    return *gauges_[name];
}

Histogram& PrometheusRegistry::register_histogram(
    const std::string& name,
    const std::vector<double>& buckets,
    const std::string& help) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = histograms_.find(name);
    if (it != histograms_.end()) {
        return *it->second;  // 返回已存在的 Histogram
    }

    MetricInfo info;
    info.help = help;
    info.type = "histogram";
    metric_infos_[name] = info;

    // 先创建空 unique_ptr，再赋值
    histograms_[name] = std::make_unique<Histogram>(buckets);
    return *histograms_[name];
}

Summary& PrometheusRegistry::register_summary(
    const std::string& name,
    const std::vector<double>& quantiles,
    const std::string& help) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = summaries_.find(name);
    if (it != summaries_.end()) {
        return *it->second;  // 返回已存在的 Summary
    }

    MetricInfo info;
    info.help = help;
    info.type = "summary";
    metric_infos_[name] = info;

    // 先创建空 unique_ptr，再赋值
    summaries_[name] = std::make_unique<Summary>(quantiles);
    return *summaries_[name];
}

std::string PrometheusRegistry::export_metrics() const {
    return to_prometheus();
}

std::string PrometheusRegistry::to_prometheus() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ostringstream oss;

    for (const auto& pair : counters_) {
        oss << pair.second->to_prometheus(pair.first,
                                          metric_infos_.at(pair.first).help);
    }

    for (const auto& pair : gauges_) {
        oss << pair.second->to_prometheus(pair.first,
                                          metric_infos_.at(pair.first).help);
    }

    for (const auto& pair : histograms_) {
        oss << pair.second->to_prometheus(pair.first,
                                          metric_infos_.at(pair.first).help);
    }

    for (const auto& pair : summaries_) {
        oss << pair.second->to_prometheus(pair.first,
                                          metric_infos_.at(pair.first).help);
    }

    return oss.str();
}

PrometheusRegistry& get_prometheus_registry() {
    static PrometheusRegistry registry;
    return registry;
}

} // namespace metrics
} // namespace chwell
