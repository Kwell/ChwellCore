#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "chwell/net/logic_thread.h"
#include "chwell/net/tcp_connection.h"

using namespace chwell;
using namespace std::chrono_literals;

namespace {

net::TcpConnectionPtr make_conn() {
    // 空 socket：仅作为分片 key（conn_id）载体
    return std::make_shared<net::TcpConnection>(net::TcpSocket());
}

TEST(LogicThreadPoolTest, WorkerCountAndDefaults) {
    net::LogicThreadPool pool(4);
    EXPECT_EQ(4u, pool.size());
    EXPECT_NE(nullptr, pool.worker(0));
    EXPECT_EQ(nullptr, pool.worker(99));

    net::LogicThreadPool zero(0);
    EXPECT_EQ(1u, zero.size());
}

TEST(LogicThreadPoolTest, SameConnRoutesToSameWorker) {
    net::LogicThreadPool pool(4);
    auto conn = make_conn();

    // pick() 私有，通过 post 的落点验证：同 conn 的消息都进同一 worker
    std::atomic<int> hits{0};
    pool.set_message_handler([&](const net::TcpConnectionPtr&, std::string_view) {
        hits.fetch_add(1);
    });
    pool.start();

    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(pool.post(conn, "hello"));
    }
    EXPECT_TRUE(pool.drain(1000));
    EXPECT_EQ(20, hits.load());
    pool.stop();
}

TEST(LogicThreadPoolTest, PostTaskForAffinity) {
    net::LogicThreadPool pool(4);
    std::atomic<int> done{0};
    pool.start();

    // 同一 key 恒定同一 worker：串行执行，计数无竞争
    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(pool.post_task_for(12345, [&done]() {
            done.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    EXPECT_TRUE(pool.drain(1000));
    EXPECT_EQ(50, done.load());
    pool.stop();
}

TEST(LogicThreadPoolTest, PostTaskRoundRobin) {
    net::LogicThreadPool pool(3);
    std::atomic<int> done{0};
    pool.start();

    for (int i = 0; i < 30; ++i) {
        EXPECT_TRUE(pool.post_task([&done]() {
            done.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    EXPECT_TRUE(pool.drain(1000));
    EXPECT_EQ(30, done.load());

    // 所有 worker 都应处理过至少一条（轮询）
    bool any = false;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        if (pool.worker(i)->total_processed() > 0) any = true;
    }
    EXPECT_TRUE(any);
    pool.stop();
}

TEST(LogicThreadPoolTest, DisconnectRoutesToSameWorker) {
    net::LogicThreadPool pool(4);
    auto conn = make_conn();
    std::atomic<int> disc{0};
    pool.set_disconnect_handler([&](const net::TcpConnectionPtr&) {
        disc.fetch_add(1);
    });
    pool.start();
    EXPECT_TRUE(pool.post_disconnect(conn));
    EXPECT_TRUE(pool.drain(1000));
    EXPECT_EQ(1, disc.load());
    pool.stop();
}

TEST(LogicThreadPoolTest, PendingAndProcessedAggregate) {
    net::LogicThreadPool pool(2);
    pool.start();
    std::atomic<int> n{0};
    pool.set_message_handler([&](const net::TcpConnectionPtr&, std::string_view) {
        n.fetch_add(1);
    });
    auto c = make_conn();
    for (int i = 0; i < 5; ++i) pool.post(c, "x");
    EXPECT_TRUE(pool.drain(1000));
    EXPECT_EQ(5, n.load());
    EXPECT_LE(pool.pending_count(), 0u);
    EXPECT_GE(pool.total_processed(), 5u);
    pool.stop();
}

}  // namespace
