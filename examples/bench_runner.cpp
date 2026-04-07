// ChwellCore Benchmark Runner — 全面性能基准测试
// 编译后独立运行: ./bench_runner

#include "chwell/benchmark/benchmark.h"
#include "chwell/protocol/message.h"
#include "chwell/protocol/parser.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/loadbalance/load_balancer.h"
#include "chwell/loadbalance/consistent_hash.h"
#include "chwell/discovery/service_discovery.h"
#include "chwell/codec/codec.h"

#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <map>
#include <unordered_map>
#include <functional>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

using namespace chwell;
using namespace chwell::benchmark;

// ============================================
// 协议 Benchmarks
// ============================================

void bench_message_serialize(size_t iterations, size_t body_size) {
    for (size_t i = 0; i < iterations; ++i) {
        protocol::Message msg(1001, std::string(body_size, 'x'));
        volatile auto data = protocol::serialize(msg);
        (void)data;
    }
}

void bench_message_deserialize(size_t iterations, size_t body_size) {
    protocol::Message original(1001, std::string(body_size, 'x'));
    auto data = protocol::serialize(original);
    for (size_t i = 0; i < iterations; ++i) {
        protocol::Message msg;
        volatile bool ok = protocol::deserialize(data, msg);
        (void)ok;
    }
}

void bench_protocol_parser_parse(size_t iterations, size_t body_size) {
    codec::ProtobufCodec codec;
    std::string payload(body_size, 'y');
    std::vector<char> combined;
    for (int i = 0; i < 10; ++i) {
        auto encoded = codec.encode(payload);
        combined.insert(combined.end(), encoded.begin(), encoded.end());
    }
    for (size_t i = 0; i < iterations; ++i) {
        auto msgs = codec.decode(combined);
        (void)msgs;
    }
}

void bench_message_create_destroy(size_t iterations, size_t body_size) {
    for (size_t i = 0; i < iterations; ++i) {
        protocol::Message msg(1001, std::string(body_size, 'z'));
    }
}

void bench_codec_encode_decode(size_t iterations, size_t body_size) {
    codec::ProtobufCodec codec;
    std::string payload(body_size, 'd');
    auto encoded = codec.encode(payload);
    for (size_t i = 0; i < iterations; ++i) {
        auto msgs = codec.decode(encoded);
        if (!msgs.empty()) {
            auto re_encoded = codec.encode(msgs[0]);
            (void)re_encoded;
        }
    }
}

void bench_length_codec(size_t iterations, size_t body_size) {
    codec::LengthHeaderCodec codec;
    std::string payload(body_size, 'l');
    auto encoded = codec.encode(payload);
    for (size_t i = 0; i < iterations; ++i) {
        auto msgs = codec.decode(encoded);
        if (!msgs.empty()) {
            auto re = codec.encode(msgs[0]);
            (void)re;
        }
    }
}

// ============================================
// 负载均衡 Benchmarks
// ============================================

void bench_round_robin(size_t iterations) {
    // 创建 discovery 和注册实例（只做一次，避免日志刷屏）
    static auto discovery = []() {
        auto d = std::make_shared<discovery::MemoryServiceDiscovery>(30000, 0);
        for (int i = 0; i < 5; ++i) {
            discovery::ServiceInstance inst;
            inst.service_id  = "bench_svc_rr";
            inst.instance_id = "rr_inst_" + std::to_string(i);
            inst.host = "127.0.0.1";
            inst.port = static_cast<uint16_t>(8000 + i);
            inst.is_alive = true;
            d->register_service(inst);
        }
        return d;
    }();
    static loadbalance::RoundRobinLoadBalancer lb(discovery);
    for (size_t i = 0; i < iterations; ++i) {
        discovery::ServiceInstance out;
        volatile bool ok = lb.select_instance("bench_svc_rr", out);
        (void)ok;
    }
}

void bench_consistent_hash(size_t iterations) {
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

void bench_weighted_round_robin(size_t iterations) {
    // 创建 discovery 和注册实例（只做一次，避免日志刷屏）
    static auto discovery = []() {
        auto d = std::make_shared<discovery::MemoryServiceDiscovery>(30000, 0);
        for (int i = 0; i < 5; ++i) {
            discovery::ServiceInstance inst;
            inst.service_id  = "bench_svc_wrr";
            inst.instance_id = "wrr_inst_" + std::to_string(i);
            inst.host = "127.0.0.1";
            inst.port = static_cast<uint16_t>(8100 + i);
            inst.is_alive = true;
            d->register_service(inst);
        }
        return d;
    }();
    static auto lb_ptr = []() {
        auto lb = std::make_shared<loadbalance::WeightedRoundRobinLoadBalancer>(discovery);
        for (int i = 0; i < 5; ++i) {
            lb->set_weight("wrr_inst_" + std::to_string(i), i + 1);
        }
        return lb;
    }();
    for (size_t i = 0; i < iterations; ++i) {
        discovery::ServiceInstance out;
        volatile bool ok = lb_ptr->select_instance("bench_svc_wrr", out);
        (void)ok;
    }
}

// ============================================
// 内存 Benchmarks
// ============================================

void bench_vector_alloc(size_t size, size_t count) {
    std::vector<int> v;
    v.reserve(size);
    for (size_t i = 0; i < count; ++i) {
        v.push_back(static_cast<int>(i));
        if (v.size() >= size) v.clear();
    }
    volatile size_t s = v.size();
    (void)s;
}

void bench_map_insert(size_t count) {
    std::map<int, int> m;
    for (size_t i = 0; i < count; ++i) {
        m[static_cast<int>(i)] = static_cast<int>(i);
    }
    volatile size_t s = m.size();
    (void)s;
}

void bench_string_concat(size_t length) {
    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += static_cast<char>('a' + (i % 26));
    }
    volatile size_t s = result.size();
    (void)s;
}

void bench_unordered_map(size_t count) {
    std::unordered_map<int, int> m;
    m.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        m[static_cast<int>(i)] = static_cast<int>(i);
    }
    volatile size_t s = m.size();
    (void)s;
}

void bench_shared_ptr_copy(size_t count) {
    auto sp = std::make_shared<std::vector<char>>(4096);
    for (size_t i = 0; i < count; ++i) {
        auto copy = sp;
        (void)copy;
    }
}

// ============================================
// TCP Benchmarks (socketpair)
// ============================================

void bench_tcp_connect() {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return;
    ::close(sv[0]);
    ::close(sv[1]);
}

void bench_tcp_send_recv() {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return;
    const char msg[] = "ping";
    char buf[8];
    ::send(sv[0], msg, sizeof(msg) - 1, 0);
    ::recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT);
    ::close(sv[0]);
    ::close(sv[1]);
}

// ============================================
// 并发 Benchmarks
// ============================================

void bench_atomic_increment(size_t iterations) {
    std::atomic<int> counter{0};
    int num_threads = 4;
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&counter, iterations, num_threads]() {
            for (size_t j = 0; j < iterations / num_threads; ++j) {
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads) t.join();
    (void)counter;
}

void bench_mutex_contention(size_t iterations) {
    std::mutex mtx;
    int value = 0;
    int num_threads = 4;
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&mtx, &value, iterations, num_threads]() {
            for (size_t j = 0; j < iterations / num_threads; ++j) {
                std::lock_guard<std::mutex> lock(mtx);
                ++value;
            }
        });
    }
    for (auto& t : threads) t.join();
    (void)value;
}

// ============================================
// 时钟 Benchmarks
// ============================================

void bench_steady_clock_now(size_t iterations) {
    for (size_t i = 0; i < iterations; ++i) {
        volatile auto now = std::chrono::steady_clock::now();
        (void)now;
    }
}

// ============================================
// Main
// ============================================

void print_suite_results(const std::string& title, const std::vector<BenchmarkResult>& results) {
    std::cout << "\n═══════════════════════════════════════════════════════════\n";
    std::cout << "  " << title << "\n";
    std::cout << "═══════════════════════════════════════════════════════════\n";
    std::cout << std::left << std::setw(35) << "Benchmark"
              << std::right << std::setw(12) << "Avg(ms)"
              << std::setw(12) << "Min(ms)"
              << std::setw(12) << "Max(ms)"
              << std::setw(14) << "Ops/sec"
              << std::setw(12) << "Iters"
              << "\n";
    std::cout << std::string(97, '-') << "\n";
    for (const auto& r : results) {
        std::cout << std::left << std::setw(35) << r.name
                  << std::right << std::fixed << std::setprecision(4)
                  << std::setw(12) << r.avg_time_ms
                  << std::setw(12) << r.min_time_ms
                  << std::setw(12) << r.max_time_ms
                  << std::setw(14) << std::setprecision(0) << r.ops_per_second
                  << std::setw(12) << r.iterations
                  << "\n";
    }
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║         ChwellCore Performance Benchmark Suite          ║\n";
    std::cout << "║              " << __DATE__ << " " << __TIME__ << "                  ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";

    BenchmarkConfig fast_config;
    fast_config.warmup_iterations = 50;
    fast_config.measurement_iterations = 1000;

    BenchmarkConfig default_config;
    default_config.warmup_iterations = 100;
    default_config.measurement_iterations = 10000;

    // ============ 协议层 ============
    {
        BenchmarkSuite suite("Protocol");
        suite.add_benchmark("serialize_100B",     []() { bench_message_serialize(1000, 100); });
        suite.add_benchmark("serialize_1KB",      []() { bench_message_serialize(1000, 1024); });
        suite.add_benchmark("serialize_10KB",     []() { bench_message_serialize(100, 10240); });
        suite.add_benchmark("deserialize_100B",   []() { bench_message_deserialize(1000, 100); });
        suite.add_benchmark("deserialize_1KB",    []() { bench_message_deserialize(1000, 1024); });
        suite.add_benchmark("deserialize_10KB",   []() { bench_message_deserialize(100, 10240); });
        suite.add_benchmark("parser_10x1KB",      []() { bench_protocol_parser_parse(1000, 1024); });
        suite.add_benchmark("create_destroy_1KB", []() { bench_message_create_destroy(10000, 1024); });
        print_suite_results("Protocol Layer", suite.run(fast_config));
    }

    // ============ 编解码器 ============
    {
        BenchmarkSuite suite("Codec");
        suite.add_benchmark("protobuf_enc_dec_1KB",  []() { bench_codec_encode_decode(1000, 1024); });
        suite.add_benchmark("protobuf_enc_dec_10KB", []() { bench_codec_encode_decode(100, 10240); });
        suite.add_benchmark("length_enc_dec_1KB",    []() { bench_length_codec(1000, 1024); });
        suite.add_benchmark("length_enc_dec_10KB",   []() { bench_length_codec(100, 10240); });
        print_suite_results("Codec Layer", suite.run(fast_config));
    }

    // ============ 负载均衡 ============
    {
        BenchmarkSuite suite("LoadBalance");
        suite.add_benchmark("round_robin",       []() { bench_round_robin(10000); });
        suite.add_benchmark("consistent_hash",   []() { bench_consistent_hash(10000); });
        suite.add_benchmark("weighted_rr",       []() { bench_weighted_round_robin(10000); });
        print_suite_results("Load Balancing", suite.run(fast_config));
    }

    // ============ 内存 ============
    {
        BenchmarkSuite suite("Memory");
        suite.add_benchmark("vector_alloc_10K",    []() { bench_vector_alloc(10000, 10000); });
        suite.add_benchmark("map_insert_1K",       []() { bench_map_insert(1000); });
        suite.add_benchmark("unordered_map_1K",    []() { bench_unordered_map(1000); });
        suite.add_benchmark("string_concat_1K",    []() { bench_string_concat(1000); });
        suite.add_benchmark("shared_ptr_copy_10K", []() { bench_shared_ptr_copy(10000); });
        print_suite_results("Memory Operations", suite.run(fast_config));
    }

    // ============ TCP ============
    {
        BenchmarkSuite suite("TCP");
        suite.add_benchmark("connect_socketpair",  bench_tcp_connect);
        suite.add_benchmark("send_recv_socketpair", bench_tcp_send_recv);
        print_suite_results("TCP Socket", suite.run(default_config));
    }

    // ============ 并发 ============
    {
        BenchmarkSuite suite("Concurrency");
        suite.add_benchmark("atomic_inc_1M",       []() { bench_atomic_increment(1000000); });
        suite.add_benchmark("mutex_contention_1M", []() { bench_mutex_contention(1000000); });
        print_suite_results("Concurrency Primitives", suite.run(fast_config));
    }

    // ============ 时钟 ============
    {
        BenchmarkSuite suite("Clock");
        suite.add_benchmark("steady_clock_1M", []() { bench_steady_clock_now(1000000); });
        print_suite_results("Timer/Clock", suite.run(fast_config));
    }

    std::cout << "\n═══════════════════════════════════════════════════════════\n";
    std::cout << "  Benchmark Complete\n";
    std::cout << "═══════════════════════════════════════════════════════════\n\n";

    return 0;
}
