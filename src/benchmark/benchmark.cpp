#include "chwell/benchmark/benchmark.h"
#include "chwell/core/logger.h"
#include "chwell/protocol/message.h"
#include "chwell/protocol/parser.h"
#include "chwell/service/protocol_router.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/loadbalance/load_balancer.h"
#include "chwell/loadbalance/consistent_hash.h"
#include "chwell/discovery/service_discovery.h"

#include <algorithm>
#include <random>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <map>
#include <sys/socket.h>
#include <unistd.h>

namespace chwell {
namespace benchmark {

// ============================================
// BenchmarkSuite 实现
// ============================================

void BenchmarkSuite::add_benchmark(
    const std::string& name,
    const std::string& description,
    BenchmarkFunction benchmark) {

    BenchmarkDescriptor desc;
    desc.name = name;
    desc.description = description;
    desc.func = benchmark;

    benchmarks_.push_back(desc);
}

void BenchmarkSuite::warmup(BenchmarkFunction func, size_t iterations) {
    for (size_t i = 0; i < iterations; ++i) {
        func();
    }
}

std::vector<double> BenchmarkSuite::measure(BenchmarkFunction func, size_t iterations) {
    std::vector<double> times;
    times.reserve(iterations);

    for (size_t i = 0; i < iterations; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        func();
        auto end = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        times.push_back(duration.count() / 1000000.0);  // 转换为毫秒
    }

    return times;
}

double BenchmarkSuite::calculate_avg(const std::vector<double>& values) {
    if (values.empty()) return 0.0;

    double sum = 0.0;
    for (double v : values) {
        sum += v;
    }
    return sum / values.size();
}

double BenchmarkSuite::calculate_min(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return *std::min_element(values.begin(), values.end());
}

double BenchmarkSuite::calculate_max(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return *std::max_element(values.begin(), values.end());
}

double BenchmarkSuite::calculate_stddev(const std::vector<double>& values, double avg) {
    if (values.empty()) return 0.0;

    double sum_sq = 0.0;
    for (double v : values) {
        double diff = v - avg;
        sum_sq += diff * diff;
    }

    return std::sqrt(sum_sq / values.size());
}

BenchmarkResult BenchmarkSuite::run_single_benchmark(
    const BenchmarkDescriptor& desc,
    const BenchmarkConfig& config) {

    BenchmarkResult result;
    result.name = desc.name;
    result.description = desc.description;

    // 预热
    if (!config.skip_warmup && config.warmup_iterations > 0) {
        warmup(desc.func, config.warmup_iterations);
    }

    // 测量
    std::vector<double> times = measure(desc.func, config.measurement_iterations);

    // 计算统计信息
    result.iterations = config.measurement_iterations;
    result.avg_time_ms = calculate_avg(times);
    result.min_time_ms = calculate_min(times);
    result.max_time_ms = calculate_max(times);
    double stddev = calculate_stddev(times, result.avg_time_ms);

    // 计算 ops/sec
    result.ops_per_second = 1000.0 / result.avg_time_ms;
    result.ops_per_second_min = 1000.0 / result.max_time_ms;
    result.ops_per_second_max = 1000.0 / result.min_time_ms;
    result.ops_per_second_stddev = stddev * result.ops_per_second / result.avg_time_ms;

    return result;
}

std::vector<BenchmarkResult> BenchmarkSuite::run(const BenchmarkConfig& config) {
    results_.clear();

    for (const auto& desc : benchmarks_) {
        BenchmarkResult result = run_single_benchmark(desc, config);
        results_.push_back(result);
    }

    return results_;
}

BenchmarkResult BenchmarkSuite::run_benchmark(
    const std::string& name,
    BenchmarkConfig config) {

    // 查找对应的 benchmark
    for (const auto& desc : benchmarks_) {
        if (desc.name == name) {
            BenchmarkResult result = run_single_benchmark(desc, config);
            results_.push_back(result);
            return result;
        }
    }

    // 没找到，返回空结果
    BenchmarkResult empty;
    empty.name = name;
    empty.iterations = 0;
    return empty;
}

std::string BenchmarkSuite::export_csv() const {
    std::ostringstream oss;

    // CSV header
    oss << "name,description,iterations,avg_time_ms,min_time_ms,max_time_ms,ops_per_second,ops_per_second_min,ops_per_second_max,ops_per_second_stddev\n";

    for (const auto& result : results_) {
        oss << result.name << ","
            << "\"" << result.description << "\","
            << result.iterations << ","
            << result.avg_time_ms << ","
            << result.min_time_ms << ","
            << result.max_time_ms << ","
            << result.ops_per_second << ","
            << result.ops_per_second_min << ","
            << result.ops_per_second_max << ","
            << result.ops_per_second_stddev << "\n";
    }

    return oss.str();
}

std::string BenchmarkSuite::export_json() const {
    std::ostringstream oss;

    oss << "[\n";
    for (size_t i = 0; i < results_.size(); ++i) {
        const auto& result = results_[i];

        oss << "  {\n";
        oss << "    \"name\": \"" << result.name << "\",\n";
        oss << "    \"description\": \"" << result.description << "\",\n";
        oss << "    \"iterations\": " << result.iterations << ",\n";
        oss << "    \"avg_time_ms\": " << result.avg_time_ms << ",\n";
        oss << "    \"min_time_ms\": " << result.min_time_ms << ",\n";
        oss << "    \"max_time_ms\": " << result.max_time_ms << ",\n";
        oss << "    \"ops_per_second\": " << result.ops_per_second << ",\n";
        oss << "    \"ops_per_second_min\": " << result.ops_per_second_min << ",\n";
        oss << "    \"ops_per_second_max\": " << result.ops_per_second_max << ",\n";
        oss << "    \"ops_per_second_stddev\": " << result.ops_per_second_stddev << "\n";

        if (i < results_.size() - 1) {
            oss << "  },\n";
        } else {
            oss << "  }\n";
        }
    }
    oss << "]\n";

    return oss.str();
}

void BenchmarkSuite::print_results() const {
    std::cout << "\n==========================================\n";
    std::cout << "Benchmark Suite: " << name_ << "\n";
    std::cout << "==========================================\n\n";

    for (const auto& result : results_) {
        std::cout << "Benchmark: " << result.name << "\n";
        if (!result.description.empty()) {
            std::cout << "  Description: " << result.description << "\n";
        }
        std::cout << "  Iterations: " << result.iterations << "\n";
        std::cout << "  Avg Time: " << std::fixed << std::setprecision(4) << result.avg_time_ms << " ms\n";
        std::cout << "  Min Time: " << std::fixed << std::setprecision(4) << result.min_time_ms << " ms\n";
        std::cout << "  Max Time: " << std::fixed << std::setprecision(4) << result.max_time_ms << " ms\n";
        std::cout << "  Ops/Sec: " << std::fixed << std::setprecision(2) << result.ops_per_second << "\n";
        std::cout << "  Ops/Sec (min): " << std::fixed << std::setprecision(2) << result.ops_per_second_min << "\n";
        std::cout << "  Ops/Sec (max): " << std::fixed << std::setprecision(2) << result.ops_per_second_max << "\n";
        std::cout << "  Ops/Sec (stddev): " << std::fixed << std::setprecision(2) << result.ops_per_second_stddev << "\n";
        std::cout << "\n";
    }
    std::cout << "==========================================\n\n";
}

// ============================================
// 具体 Benchmarks 实现
// ============================================

namespace tcp_bench {

void benchmark_tcp_connect() {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        CHWELL_LOG_ERROR("benchmark_tcp_connect: socketpair failed");
        return;
    }
    ::close(sv[0]);
    ::close(sv[1]);
}

void benchmark_tcp_send_receive() {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        CHWELL_LOG_ERROR("benchmark_tcp_send_receive: socketpair failed");
        return;
    }
    const char msg[] = "ping";
    char buf[8];
    ::send(sv[0], msg, sizeof(msg) - 1, 0);
    ::recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT);
    ::close(sv[0]);
    ::close(sv[1]);
}

void benchmark_concurrent_connections(int num_connections) {
    std::vector<int> fds;
    fds.reserve(num_connections * 2);
    for (int i = 0; i < num_connections; ++i) {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) break;
        fds.push_back(sv[0]);
        fds.push_back(sv[1]);
    }
    for (int fd : fds) ::close(fd);
}

} // namespace tcp_bench

namespace memory_bench {

void benchmark_vector_alloc(size_t size, size_t count) {
    std::vector<int> v;
    v.reserve(size);
    for (size_t i = 0; i < count; ++i) {
        v.push_back(static_cast<int>(i));
        if (v.size() >= size) v.clear();
    }
    volatile size_t s = v.size();
    (void)s;
}

void benchmark_map_insert(size_t count) {
    std::map<int, int> m;
    for (size_t i = 0; i < count; ++i) {
        m[static_cast<int>(i)] = static_cast<int>(i);
    }
    volatile size_t s = m.size();
    (void)s;
}

void benchmark_string_concat(size_t length) {
    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += static_cast<char>('a' + (i % 26));
    }
    volatile size_t s = result.size();
    (void)s;
}

} // namespace memory_bench

namespace loadbalance_bench {

void benchmark_round_robin_select(size_t iterations) {
    auto discovery = std::make_shared<discovery::MemoryServiceDiscovery>(30000, 0);
    for (int i = 0; i < 5; ++i) {
        discovery::ServiceInstance inst;
        inst.service_id  = "bench_svc";
        inst.instance_id = "inst_" + std::to_string(i);
        inst.host        = "127.0.0.1";
        inst.port        = static_cast<uint16_t>(8000 + i);
        inst.is_alive    = true;
        discovery->register_service(inst);
    }
    loadbalance::RoundRobinLoadBalancer lb(discovery);
    for (size_t i = 0; i < iterations; ++i) {
        discovery::ServiceInstance out;
        volatile bool ok = lb.select_instance("bench_svc", out);
        (void)ok;
    }
}

void benchmark_consistent_hash_select(size_t iterations) {
    loadbalance::ConsistentHashLoadBalancer lb(160);
    for (int i = 0; i < 5; ++i) {
        lb.add_instance("bench_svc", "inst_" + std::to_string(i), 1);
    }
    for (size_t i = 0; i < iterations; ++i) {
        std::string out_id;
        volatile bool ok = lb.select_instance("bench_svc", "key_" + std::to_string(i), out_id);
        (void)ok;
    }
}

void benchmark_weighted_round_robin_select(size_t iterations) {
    auto discovery = std::make_shared<discovery::MemoryServiceDiscovery>(30000, 0);
    for (int i = 0; i < 5; ++i) {
        discovery::ServiceInstance inst;
        inst.service_id  = "bench_svc";
        inst.instance_id = "inst_" + std::to_string(i);
        inst.host        = "127.0.0.1";
        inst.port        = static_cast<uint16_t>(8000 + i);
        inst.is_alive    = true;
        discovery->register_service(inst);
    }
    loadbalance::WeightedRoundRobinLoadBalancer lb(discovery);
    for (int i = 0; i < 5; ++i) {
        lb.set_weight("inst_" + std::to_string(i), i + 1);
    }
    for (size_t i = 0; i < iterations; ++i) {
        discovery::ServiceInstance out;
        volatile bool ok = lb.select_instance("bench_svc", out);
        (void)ok;
    }
}

} // namespace loadbalance_bench

namespace protocol_bench {

// 协议消息序列化基准测试
void benchmark_message_serialize(size_t iterations, size_t body_size) {
    for (size_t i = 0; i < iterations; ++i) {
        protocol::Message msg(1001, std::string(body_size, 'x'));
        volatile auto data = protocol::serialize(msg);
        (void)data;  // 防止被优化掉
    }
}

// 协议消息反序列化基准测试
void benchmark_message_deserialize(size_t iterations, size_t body_size) {
    // 预先生成消息数据
    protocol::Message original_msg(1001, std::string(body_size, 'x'));
    std::vector<char> data = protocol::serialize(original_msg);

    for (size_t i = 0; i < iterations; ++i) {
        protocol::Message msg;
        volatile bool success = protocol::deserialize(data, msg);
        (void)success;  // 防止被优化掉
    }
}

// 协议解析器解析基准测试
void benchmark_protocol_parser_parse(size_t iterations, size_t body_size) {
    // 预先生成多个消息数据（模拟粘包）
    std::vector<char> buffer;

    for (size_t i = 0; i < 10; ++i) {
        protocol::Message msg(1000 + i, std::string(body_size, 'x'));
        auto serialized = protocol::serialize(msg);
        buffer.insert(buffer.end(), serialized.begin(), serialized.end());
    }

    for (size_t i = 0; i < iterations; ++i) {
        protocol::Parser parser;
        std::vector<protocol::Message> messages = parser.feed(buffer);
        volatile size_t count = messages.size();
        (void)count;  // 防止被优化掉
    }
}

// 协议路由器分发基准测试
// iterations：每次函数调用中执行的分发次数（测量路由查找 + handler 调用延迟）
// handlers_count：注册的 handler 数量（衡量哈希表规模对查找的影响）
void benchmark_protocol_router_dispatch(size_t iterations, size_t handlers_count) {
    service::ProtocolRouterComponent router;

    for (size_t i = 0; i < handlers_count; ++i) {
        router.register_handler(static_cast<std::uint16_t>(1000 + i),
            [](const net::TcpConnectionPtr&, const protocol::Message&) {});
    }

    // 序列化一条消息（循环分发到第一个注册的 handler）
    protocol::Message msg(static_cast<std::uint16_t>(1000), std::string(100, 'x'));
    std::vector<char> raw = protocol::serialize(msg);

    // 使用空连接作为 key（不解引用）
    net::TcpConnectionPtr bench_conn;

    for (size_t i = 0; i < iterations; ++i) {
        router.on_message(bench_conn, raw);
    }
}

// 消息创建销毁基准测试
void benchmark_message_create_destroy(size_t iterations, size_t body_size) {
    for (size_t i = 0; i < iterations; ++i) {
        {
            protocol::Message msg(1001, std::string(body_size, 'x'));
            (void)msg;  // 防止被优化掉
        }
        // msg 在这里销毁
    }
}

// 消息拷贝/移动基准测试
void benchmark_message_copy_move(size_t iterations, size_t body_size) {
    std::vector<protocol::Message> messages;

    // 创建初始消息
    for (size_t i = 0; i < 100; ++i) {
        messages.emplace_back(1000 + i, std::string(body_size, 'x'));
    }

    for (size_t i = 0; i < iterations; ++i) {
        std::vector<protocol::Message> temp_messages;

        // 拷贝
        for (const auto& msg : messages) {
            temp_messages.push_back(msg);  // 拷贝构造
        }

        // 移动
        std::vector<protocol::Message> moved_messages;
        for (auto& msg : temp_messages) {
            moved_messages.push_back(std::move(msg));  // 移动构造
        }
    }
}

} // namespace protocol_bench

} // namespace benchmark
} // namespace chwell
