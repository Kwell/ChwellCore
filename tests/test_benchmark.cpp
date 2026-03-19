#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <chrono>
#include <random>

#include "chwell/benchmark/benchmark.h"
#include "chwell/protocol/message.h"
#include "chwell/service/protocol_router.h"

using namespace chwell;
using namespace chwell::benchmark;

// ============================================
// Benchmarks 单元测试
// ============================================

// 简单的基准测试：空循环
void simple_benchmark() {
    volatile int sum = 0;
    for (volatile int i = 0; i < 1000; ++i) {
        sum += i;
    }
}

// 向量分配基准
void benchmark_vector_alloc() {
    std::vector<int> vec;
    for (size_t i = 0; i < 10000; ++i) {
        vec.push_back(i);
    }
}

// 字符串拼接基准
void benchmark_string_concat() {
    std::string result;
    for (int i = 0; i < 1000; ++i) {
        result += "hello";
    }
}

// Map 插入基准
void benchmark_map_insert() {
    std::map<int, int> m;
    for (int i = 0; i < 1000; ++i) {
        m[i] = i * 2;
    }
}

// 并发基准
void benchmark_concurrent_atomic_inc() {
    std::atomic<int> counter{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&counter]() {
            for (int j = 0; j < 10000; ++j) {
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
}

// ============================================
// Benchmarks 测试
// ============================================

TEST(BenchmarkTest, SimpleBenchmark) {
    BenchmarkSuite suite("Simple Benchmarks");

    suite.add_benchmark("empty_loop", "Empty loop 1000 iterations", []() {
        volatile int sum = 0;
        for (volatile int i = 0; i < 1000; ++i) {
            sum += i;
        }
    });

    BenchmarkConfig config;
    config.warmup_iterations = 1000;
    config.measurement_iterations = 100000;

    auto results = suite.run(config);

    EXPECT_FALSE(results.empty());
    EXPECT_EQ(1u, results.size());
}

TEST(BenchmarkTest, VectorAlloc) {
    BenchmarkSuite suite("Memory Benchmarks");

    suite.add_benchmark("vector_alloc_10000", "Vector of 10000 ints", benchmark_vector_alloc);

    BenchmarkConfig config;
    config.warmup_iterations = 100;
    config.measurement_iterations = 1000;

    auto results = suite.run(config);

    EXPECT_FALSE(results.empty());
}

TEST(BenchmarkTest, StringConcat) {
    BenchmarkSuite suite("String Benchmarks");

    suite.add_benchmark("string_concat_1000", "String concat 1000 times", benchmark_string_concat);

    BenchmarkConfig config;
    config.warmup_iterations = 100;
    config.measurement_iterations = 1000;

    auto results = suite.run(config);

    EXPECT_FALSE(results.empty());
}

TEST(BenchmarkTest, MapInsert) {
    BenchmarkSuite suite("Data Structure Benchmarks");

    suite.add_benchmark("map_insert_1000", "Map insert 1000 items", benchmark_map_insert);

    BenchmarkConfig config;
    config.warmup_iterations = 100;
    config.measurement_iterations = 100;

    auto results = suite.run(config);

    EXPECT_FALSE(results.empty());
}

TEST(BenchmarkTest, ConcurrentAtomicInc) {
    BenchmarkSuite suite("Concurrency Benchmarks");

    suite.add_benchmark("concurrent_atomic_inc_4_threads", "Concurrent atomic inc 4 threads 10000 iters", benchmark_concurrent_atomic_inc);

    BenchmarkConfig config;
    config.warmup_iterations = 10;
    config.measurement_iterations = 1000;

    auto results = suite.run(config);

    EXPECT_FALSE(results.empty());
}

// ============================================
// Protocol Benchmarks
// ============================================

TEST(BenchmarkTest, ProtocolMessageSerialize) {
    BenchmarkSuite suite("Protocol Benchmarks");

    suite.add_benchmark("message_serialize_100bytes",
                        "Serialize message with 100 byte body",
                        []() {
        protocol_bench::benchmark_message_serialize(1000, 100);
    });

    suite.add_benchmark("message_serialize_1kbytes",
                        "Serialize message with 1K byte body",
                        []() {
        protocol_bench::benchmark_message_serialize(1000, 1024);
    });

    suite.add_benchmark("message_serialize_10kbytes",
                        "Serialize message with 10K byte body",
                        []() {
        protocol_bench::benchmark_message_serialize(100, 10240);
    });

    BenchmarkConfig config;
    config.warmup_iterations = 100;
    config.measurement_iterations = 10000;

    auto results = suite.run(config);
    suite.print_results();

    EXPECT_EQ(3u, results.size());
}

TEST(BenchmarkTest, ProtocolMessageDeserialize) {
    BenchmarkSuite suite("Protocol Benchmarks");

    suite.add_benchmark("message_deserialize_100bytes",
                        "Deserialize message with 100 byte body",
                        []() {
        protocol_bench::benchmark_message_deserialize(1000, 100);
    });

    suite.add_benchmark("message_deserialize_1kbytes",
                        "Deserialize message with 1K byte body",
                        []() {
        protocol_bench::benchmark_message_deserialize(1000, 1024);
    });

    suite.add_benchmark("message_deserialize_10kbytes",
                        "Deserialize message with 10K byte body",
                        []() {
        protocol_bench::benchmark_message_deserialize(100, 10240);
    });

    BenchmarkConfig config;
    config.warmup_iterations = 100;
    config.measurement_iterations = 10000;

    auto results = suite.run(config);
    suite.print_results();

    EXPECT_EQ(3u, results.size());
}

TEST(BenchmarkTest, ProtocolParserParse) {
    BenchmarkSuite suite("Protocol Parser Benchmarks");

    suite.add_benchmark("parser_parse_10msg_100bytes",
                        "Parse 10 messages of 100 bytes each",
                        []() {
        protocol_bench::benchmark_protocol_parser_parse(1000, 100);
    });

    suite.add_benchmark("parser_parse_10msg_1kbytes",
                        "Parse 10 messages of 1K bytes each",
                        []() {
        protocol_bench::benchmark_protocol_parser_parse(100, 1024);
    });

    BenchmarkConfig config;
    config.warmup_iterations = 50;
    config.measurement_iterations = 1000;

    auto results = suite.run(config);
    suite.print_results();

    EXPECT_EQ(2u, results.size());
}

TEST(BenchmarkTest, ProtocolRouterDispatch) {
    BenchmarkSuite suite("Protocol Router Benchmarks");

    suite.add_benchmark("router_dispatch_10handlers",
                        "Dispatch to 10 handlers",
                        []() {
        protocol_bench::benchmark_protocol_router_dispatch(100, 10);
    });

    suite.add_benchmark("router_dispatch_100handlers",
                        "Dispatch to 100 handlers",
                        []() {
        protocol_bench::benchmark_protocol_router_dispatch(100, 100);
    });

    suite.add_benchmark("router_dispatch_1000handlers",
                        "Dispatch to 1000 handlers",
                        []() {
        protocol_bench::benchmark_protocol_router_dispatch(10, 1000);
    });

    BenchmarkConfig config;
    config.warmup_iterations = 50;
    config.measurement_iterations = 1000;

    auto results = suite.run(config);
    suite.print_results();

    EXPECT_EQ(3u, results.size());
}

TEST(BenchmarkTest, MessageCreateDestroy) {
    BenchmarkSuite suite("Message Lifecycle Benchmarks");

    suite.add_benchmark("message_create_destroy_100bytes",
                        "Create/destroy message with 100 byte body",
                        []() {
        protocol_bench::benchmark_message_create_destroy(1000, 100);
    });

    suite.add_benchmark("message_create_destroy_1kbytes",
                        "Create/destroy message with 1K byte body",
                        []() {
        protocol_bench::benchmark_message_create_destroy(1000, 1024);
    });

    BenchmarkConfig config;
    config.warmup_iterations = 100;
    config.measurement_iterations = 10000;

    auto results = suite.run(config);
    suite.print_results();

    EXPECT_EQ(2u, results.size());
}

TEST(BenchmarkTest, MessageCopyMove) {
    BenchmarkSuite suite("Message Copy/Move Benchmarks");

    suite.add_benchmark("message_copy_move_100bytes",
                        "Copy/move message with 100 byte body",
                        []() {
        protocol_bench::benchmark_message_copy_move(100, 100);
    });

    suite.add_benchmark("message_copy_move_1kbytes",
                        "Copy/move message with 1K byte body",
                        []() {
        protocol_bench::benchmark_message_copy_move(100, 1024);
    });

    BenchmarkConfig config;
    config.warmup_iterations = 50;
    config.measurement_iterations = 1000;

    auto results = suite.run(config);
    suite.print_results();

    EXPECT_EQ(2u, results.size());
}

TEST(BenchmarkTest, ProtocolFullSuite) {
    BenchmarkSuite suite("Protocol Full Suite");

    // 添加所有协议相关的 benchmark
    suite.add_benchmark("serialize_1k", "Serialize 1K message", []() {
        protocol_bench::benchmark_message_serialize(100, 1024);
    });

    suite.add_benchmark("deserialize_1k", "Deserialize 1K message", []() {
        protocol_bench::benchmark_message_deserialize(100, 1024);
    });

    suite.add_benchmark("parser_10x1k", "Parse 10x1K messages", []() {
        protocol_bench::benchmark_protocol_parser_parse(100, 1024);
    });

    suite.add_benchmark("create_destroy_1k", "Create/destroy 1K message", []() {
        protocol_bench::benchmark_message_create_destroy(1000, 1024);
    });

    suite.add_benchmark("copy_move_1k", "Copy/move 1K message", []() {
        protocol_bench::benchmark_message_copy_move(100, 1024);
    });

    BenchmarkConfig config;
    config.warmup_iterations = 50;
    config.measurement_iterations = 1000;

    auto results = suite.run(config);
    suite.print_results();

    // 导出结果
    std::cout << "\n=== CSV Export ===\n";
    std::cout << suite.export_csv() << "\n";

    EXPECT_EQ(5u, results.size());
}
