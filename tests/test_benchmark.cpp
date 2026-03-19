#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <chrono>
#include <random>

#include "chwell/benchmark/benchmark.h"

using namespace chwell;

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

    auto results = suite(config);

    EXPECT_FALSE(results.empty());
}

TEST(BenchmarkTest, ConcurrentAtomicInc) {
    BenchmarkSuite suite("Concurrency Benchmarks");

    suite.add_benchmark("concurrent_atomic_inc_4_threads", "Concurrent atomic inc 4 threads 10000 iters", benchmark_concurrent_atomic_inc);

    BenchmarkConfig config;
    config.warmup_iterations = 10;
    config.meception_iterations = 1000;

    auto results = suite.run(config);

    EXPECT_FALSE(results.empty());
}
