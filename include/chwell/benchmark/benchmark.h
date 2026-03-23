#pragma once

#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace chwell {
namespace benchmark {

// Benchmark 结果
struct BenchmarkResult {
    std::string name;
    std::string description;
    size_t iterations;
    double avg_time_ms;
    double min_time_ms;
    double max_time_ms;
    double ops_per_second;
    double ops_per_second_min;
    double ops_per_second_max;
    double ops_per_second_stddev;
};

// Benchmark 配置
struct BenchmarkConfig {
    size_t warmup_iterations = 1000;  // 预热迭代次数
    size_t measurement_iterations = 10000;  // 测量迭代次数
    bool skip_warmup = false;  // 跳过预热
};

// 基准测试函数
using BenchmarkFunction = std::function<void()>;

// Benchmark 描述（内部使用）
struct BenchmarkDescriptor {
    std::string name;
    std::string description;
    BenchmarkFunction func;
};

// Benchmark Suite
class BenchmarkSuite {
public:
    explicit BenchmarkSuite(const std::string& name)
        : name_(name) {}

    // 添加基准测试
    void add_benchmark(const std::string& name,
                        const std::string& description,
                        BenchmarkFunction benchmark);

    void add_benchmark(const std::string& name,
                        BenchmarkFunction benchmark) {
        add_benchmark(name, "", benchmark);
    }

    // 运行所有基准测试
    std::vector<BenchmarkResult> run(const BenchmarkConfig& config = BenchmarkConfig());

    // 运行单个基准测试
    BenchmarkResult run_benchmark(const std::string& name,
                                   BenchmarkConfig config = BenchmarkConfig());

    // 导出结果为 CSV
    std::string export_csv() const;

    // 导出结果为 JSON
    std::string export_json() const;

    // 打印结果
    void print_results() const;

private:
    std::string name_;
    std::vector<BenchmarkResult> results_;
    std::vector<BenchmarkDescriptor> benchmarks_;

    // 内部工具函数
    BenchmarkResult run_single_benchmark(const BenchmarkDescriptor& desc,
                                          const BenchmarkConfig& config);

    void warmup(BenchmarkFunction func, size_t iterations);

    std::vector<double> measure(BenchmarkFunction func, size_t iterations);

    double calculate_avg(const std::vector<double>& values);

    double calculate_min(const std::vector<double>& values);

    double calculate_max(const std::vector<double>& values);

    double calculate_stddev(const std::vector<double>& values, double avg);
};

// Benchmarks 命名空间 - 放置具体的基准测试

// TCP 连接基准
namespace tcp_bench {
    void benchmark_tcp_connect();
    void benchmark_tcp_send_receive();
    void benchmark_concurrent_connections(int num_connections);
}

// 内存分配基准
namespace memory_bench {
    void benchmark_vector_alloc(size_t size, size_t count);
    void benchmark_map_insert(size_t count);
    void benchmark_string_concat(size_t length);
}

// 负载均衡基准
namespace loadbalance_bench {
    void benchmark_round_robin_select(size_t iterations);
    void benchmark_consistent_hash_select(size_t iterations);
    void benchmark_weighted_round_robin_select(size_t iterations);
}

// 协议基准
namespace protocol_bench {
    void benchmark_message_serialize(size_t iterations, size_t body_size);
    void benchmark_message_deserialize(size_t iterations, size_t body_size);
    void benchmark_protocol_parser_parse(size_t iterations, size_t body_size);
    void benchmark_protocol_router_dispatch(size_t iterations, size_t handlers_count);
    void benchmark_message_create_destroy(size_t iterations, size_t body_size);
    void benchmark_message_copy_move(size_t iterations, size_t body_size);
}

} // namespace benchmark
} // namespace chwell
