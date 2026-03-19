#include <gtest/gtest.h>
#include <string>

#include "chwell/metrics/prometheus_metrics.h"

using namespace chwell;

// ============================================
// Prometheus Metrics 单元测试
// ============================================

TEST(PrometheusMetricsTest, Counter) {
    metrics::Counter& counter =
        metrics::get_prometheus_registry().register_counter("test_counter", "A test counter");

    counter.inc();
    EXPECT_EQ(1.0, counter.get());

    counter.inc(5.0);
    EXPECT_EQ(6.0, counter.get());

    counter.inc(-1.0);  // Counter 也可以减（虽然不推荐）
    EXPECT_EQ(5.0, counter.get());
}

TEST(PrometheusMetricsTest, Gauge) {
    metrics::Gauge& gauge =
        metrics::get_prometheus_registry().register_gauge("test_gauge", "A test gauge");

    gauge.inc();
    EXPECT_EQ(1.0, gauge.get());

    gauge.dec();
    EXPECT_EQ(0.0, gauge.get());

    gauge.set(42.0);
    EXPECT_EQ(42.0, gauge.get());
}

TEST(PrometheusMetricsTest, Histogram) {
    std::vector<double> buckets = {0.1, 0.5, 1.0, 5.0, 10.0};
    metrics::Histogram& histogram =
        metrics::get_prometheus_registry().register_histogram("test_histogram_unique", buckets, "A test histogram");

    histogram.observe(0.05);
    histogram.observe(0.2);
    histogram.observe(0.8);
    histogram.observe(2.0);
    histogram.observe(8.0);

    EXPECT_EQ(5, histogram.get_count());
    EXPECT_DOUBLE_EQ(11.05, histogram.get_sum());

    // 检查 bucket 分布
    auto counts = histogram.get_counts();
    EXPECT_EQ(5u, counts.size());
}

TEST(PrometheusMetricsTest, Summary) {
    std::vector<double> quantiles = {0.5, 0.9, 0.99};
    metrics::Summary& summary =
        metrics::get_prometheus_registry().register_summary("test_summary_unique", quantiles, "A test summary");

    for (int i = 0; i < 100; ++i) {
        summary.observe(i);
    }

    EXPECT_EQ(100, summary.get_count());
    EXPECT_EQ(4950, summary.get_sum());

    auto qs = summary.get_quantiles();
    EXPECT_EQ(3u, qs.size());
}

TEST(PrometheusMetricsTest, PrometheusExport) {
    auto& registry = metrics::get_prometheus_registry();

    metrics::Counter& counter = registry.register_counter("requests_total", "Total requests");
    metrics::Gauge& gauge = registry.register_gauge("active_connections", "Active connections");
    std::vector<double> buckets = {0.1, 1.0, 10.0};
    metrics::Histogram& histogram = registry.register_histogram("request_duration", buckets, "Request duration");
    std::vector<double> quantiles = {0.5, 0.95, 0.99};
    metrics::Summary& summary = registry.register_summary("response_size", quantiles, "Response size");

    counter.inc(100);
    gauge.set(10);
    histogram.observe(0.05);
    histogram.observe(5.0);
    summary.observe(1024);
    summary.observe(2048);

    std::string output = registry.export_metrics();

    // 检查输出包含关键内容
    EXPECT_NE(std::string::npos, output.find("requests_total"));
    EXPECT_NE(std::string::npos, output.find("active_connections"));
    EXPECT_NE(std::string::npos, output.find("request_duration"));
    EXPECT_NE(std::string::npos, output.find("response_size"));
}

TEST(PrometheusMetricsTest, MultipleRegistrations) {
    auto& registry = metrics::get_prometheus_registry();

    // 注册同一个 Counter 两次，应该返回同一个
    metrics::Counter& counter1 = registry.register_counter("test_counter");
    metrics::Counter& counter2 = registry.register_counter("test_counter");

    counter1.inc(10);

    // 应该是同一个 Counter（指针地址相同）
    EXPECT_EQ(&counter1, &counter2);
    EXPECT_EQ(10.0, counter2.get());
}
