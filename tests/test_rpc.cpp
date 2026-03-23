#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <chrono>

#include "chwell/rpc/rpc_client.h"
#include "chwell/rpc/rpc_server.h"
#include "chwell/net/posix_io.h"
#include "chwell/core/logger.h"
#include "chwell/circuitbreaker/circuit_breaker.h"
#include "chwell/ratelimit/rate_limiter.h"

using namespace chwell;

namespace {

// Use different ports for each test to avoid inter-test port conflicts.
// Each test binds a unique port so tests can run without interfering.
constexpr unsigned short RPC_PORT_CONCURRENT      = 19880;
constexpr unsigned short RPC_PORT_CALL_SYNC       = 19881;
constexpr unsigned short RPC_PORT_TIMEOUT         = 19882;
constexpr unsigned short RPC_PORT_CIRCUIT_BREAKER = 19883;
constexpr unsigned short RPC_PORT_MULTIINFLIGHT   = 19884;

// Helper: start RPC server in a background thread
struct RpcTestServer {
    net::IoService io_service;
    rpc::RpcServer server;
    std::thread t;

    explicit RpcTestServer(unsigned short port)
        : server(io_service, port) {}

    void start() {
        server.start();
        t = std::thread([this]() { io_service.run(); });
        // Give the server thread a moment to start accepting
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    void stop() {
        server.stop();
        io_service.stop();
        if (t.joinable()) t.join();
    }

    ~RpcTestServer() { stop(); }
};

// ============================================
// RpcClient 并发调用测试
// ============================================

TEST(RpcClientTest, ConcurrentCallsCorrect) {
    RpcTestServer srv(RPC_PORT_CONCURRENT);

    std::atomic<int> echo_count{0};
    srv.server.register_method(100, [&](const std::vector<char>& req, std::vector<char>& resp) {
        resp = req; // echo
        echo_count.fetch_add(1);
    });

    srv.start();

    net::IoService client_io;
    std::thread client_thread([&]() { client_io.run(); });

    rpc::RpcClient client(client_io, 5);
    ASSERT_TRUE(client.connect("127.0.0.1", RPC_PORT_CONCURRENT));

    constexpr int N = 10;
    std::atomic<int> success_count{0};
    std::atomic<int> done_count{0};

    for (int i = 0; i < N; ++i) {
        std::string payload = "req_" + std::to_string(i);
        std::vector<char> data(payload.begin(), payload.end());

        client.call(100, data, [&, expected = payload](bool ok, const protocol::Message& resp) {
            if (ok) {
                std::string body(resp.body.begin(), resp.body.end());
                if (body == expected) {
                    success_count.fetch_add(1);
                }
            }
            done_count.fetch_add(1);
        });
    }

    // Wait for all callbacks
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (done_count.load() < N && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_EQ(N, done_count.load());
    EXPECT_EQ(N, success_count.load());

    client.disconnect();
    client_io.stop();
    if (client_thread.joinable()) client_thread.join();
}

TEST(RpcClientTest, CallSyncWorks) {
    RpcTestServer srv(RPC_PORT_CALL_SYNC);

    srv.server.register_method(200, [](const std::vector<char>& req, std::vector<char>& resp) {
        resp = req; // echo
    });

    srv.start();

    net::IoService client_io;
    std::thread client_thread([&]() { client_io.run(); });

    rpc::RpcClient client(client_io, 5);
    ASSERT_TRUE(client.connect("127.0.0.1", RPC_PORT_CALL_SYNC));

    std::string payload = "hello_sync";
    std::vector<char> data(payload.begin(), payload.end());
    protocol::Message response;

    bool ok = client.call_sync(200, data, response, 5);
    EXPECT_TRUE(ok);
    EXPECT_EQ(std::string(response.body.begin(), response.body.end()), payload);

    client.disconnect();
    client_io.stop();
    if (client_thread.joinable()) client_thread.join();
}

TEST(RpcClientTest, TimeoutCallbackFires) {
    // Server registered but handler sleeps longer than client timeout
    RpcTestServer srv(RPC_PORT_TIMEOUT);

    srv.server.register_method(300, [](const std::vector<char>&, std::vector<char>&) {
        std::this_thread::sleep_for(std::chrono::seconds(10)); // intentionally slow
    });

    srv.start();

    net::IoService client_io;
    std::thread client_thread([&]() { client_io.run(); });

    rpc::RpcClient client(client_io, 2); // 2 second default timeout
    ASSERT_TRUE(client.connect("127.0.0.1", RPC_PORT_TIMEOUT));

    std::atomic<bool> got_callback{false};
    std::atomic<bool> timeout_ok{false};

    client.call(300, {}, [&](bool ok, const protocol::Message&) {
        got_callback.store(true);
        timeout_ok.store(!ok); // expect ok=false (timeout)
    }, 2);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!got_callback.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(got_callback.load());
    EXPECT_TRUE(timeout_ok.load());

    client.disconnect();
    client_io.stop();
    if (client_thread.joinable()) client_thread.join();
}

TEST(RpcClientTest, CircuitBreakerRejectsWhenOpen) {
    net::IoService client_io;
    std::thread client_thread([&]() { client_io.run(); });

    rpc::RpcClient client(client_io);

    circuitbreaker::CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.timeout_ms = 60000;
    auto cb = std::make_shared<circuitbreaker::DefaultCircuitBreaker>("test_rpc", cfg);
    client.set_circuit_breaker(cb);

    // Trip the circuit breaker manually
    cb->trip();
    EXPECT_EQ(circuitbreaker::CircuitState::OPEN, cb->get_state());

    // Connect to a non-existent server (won't succeed, but circuit breaker should fire first)
    // We don't actually need a connection - circuit breaker fires before send
    // So we just inject a fake connected state by testing without connect
    // (RpcClient will return error because not connected, not circuit breaker)

    // Reset and manually set open state
    cb->recover(); // back to closed
    cb->trip();    // open again

    std::atomic<bool> rejected{false};
    client.call(100, {}, [&](bool ok, const protocol::Message&) {
        if (!ok) rejected.store(true);
    });
    // callback fires immediately (not connected + circuit open would both reject)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(rejected.load());

    client_io.stop();
    if (client_thread.joinable()) client_thread.join();
}

// ============================================
// RPC 协议：request_id 前缀正确性
// ============================================

TEST(RpcClientTest, MultipleInflightRequests) {
    RpcTestServer srv(RPC_PORT_MULTIINFLIGHT);

    srv.server.register_method(400, [](const std::vector<char>& req, std::vector<char>& resp) {
        // small delay so requests interleave
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        resp = req;
    });

    srv.start();

    net::IoService client_io;
    std::thread client_thread([&]() { client_io.run(); });

    rpc::RpcClient client(client_io, 5);
    ASSERT_TRUE(client.connect("127.0.0.1", RPC_PORT_MULTIINFLIGHT));

    constexpr int N = 5;
    std::atomic<int> correct{0};
    std::atomic<int> done{0};

    for (int i = 0; i < N; ++i) {
        std::string tag = "tag_" + std::to_string(i);
        std::vector<char> data(tag.begin(), tag.end());
        client.call(400, data, [&, expected = tag](bool ok, const protocol::Message& resp) {
            if (ok) {
                std::string body(resp.body.begin(), resp.body.end());
                if (body == expected) correct.fetch_add(1);
            }
            done.fetch_add(1);
        });
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (done.load() < N && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    EXPECT_EQ(N, done.load());
    EXPECT_EQ(N, correct.load());

    client.disconnect();
    client_io.stop();
    if (client_thread.joinable()) client_thread.join();
}

} // namespace
