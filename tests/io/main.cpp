#include "chwell/net/io_service.h"
#include "chwell/net/io_demuxer.h"
#include "chwell/net/epoll_demuxer.h"
#include "chwell/net/epoll_connection.h"
#include "chwell/net/epoll_server.h"
#include "chwell/net/socket_io.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/net/tcp_server.h"
#include "chwell/net/ws_connection.h"
#include "chwell/net/ws_server.h"
#include "chwell/net/tls.h"
#include "chwell/net/connection_pool.h"
#include "chwell/http/http_server.h"
#include "chwell/cluster/node.h"
#include "chwell/rpc/rpc_client.h"
#include "chwell/rpc/rpc_server.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

// These headers must compile without POSIX system headers on Windows as well.
static_assert(!std::is_copy_constructible<chwell::net::TcpSocket>::value, "socket owns its descriptor");
static_assert(!std::is_copy_constructible<chwell::net::TcpAcceptor>::value, "acceptor owns its descriptor");
static_assert(std::is_abstract<chwell::net::IoDemuxer>::value, "readiness interface is abstract");
static_assert(std::is_base_of<chwell::net::IoDemuxer, chwell::net::EpollDemuxer>::value,
              "epoll implements the readiness interface");
static_assert(std::is_constructible<chwell::net::EpollTcpConnection, int, chwell::net::IoDemuxer*>::value,
              "connection accepts any readiness backend");
using chwell::net::IoService;
using namespace std::chrono_literals;
namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
}
int main() try {
    // One runner preserves queue order, including posts made by a callback.
    {
        IoService io;
        std::vector<int> order;
        io.post({});
        io.post([&] { order.push_back(1); io.post([&] { order.push_back(3); io.stop(); }); });
        io.post([&] { order.push_back(2); });
        io.run();
        check(order == std::vector<int>({1, 2, 3}), "queue order or recursive post changed");
    }
    // Multiple workers execute each item once. Promise completion is bounded by CTest.
    {
        IoService io;
        constexpr int size = 2000;
        std::vector<std::atomic<int>> visits(size);
        for (auto& entry : visits) entry.store(0);
        std::atomic<int> finished{0};
        std::promise<void> complete;
        auto done = complete.get_future();
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i) workers.emplace_back([&] { io.run(); });
        std::vector<std::thread> producers;
        for (int p = 0; p < 4; ++p) producers.emplace_back([&, p] {
            for (int i = p; i < size; i += 4) io.post([&, i] {
                visits[i].fetch_add(1);
                if (finished.fetch_add(1) + 1 == size) complete.set_value();
            });
        });
        for (auto& producer : producers) producer.join();
        const bool ready = done.wait_for(5s) == std::future_status::ready;
        io.stop();
        for (auto& worker : workers) worker.join();
        check(ready, "workers did not finish posted work");
        for (auto& entry : visits) check(entry.load() == 1, "work lost or executed twice");
    }
    // stop wakes idle runners across transitions into the condition-variable wait.
    for (int cycle = 0; cycle < 200; ++cycle) {
        IoService io;
        std::vector<std::thread> workers;
        for (int i = 0; i < 3; ++i) workers.emplace_back([&] { io.run(); });
        std::this_thread::yield();
        io.stop();
        io.stop();
        for (auto& worker : workers) worker.join();
        check(true, "idle runners did not stop");
    }
    // Stopping leaves queued work unexecuted while in-flight work may finish.
    {
        IoService io;
        std::promise<void> entered, release;
        auto gate = release.get_future();
        auto started = entered.get_future();
        std::atomic<int> queued{0}, finished{0};
        io.post([&] { entered.set_value(); gate.wait(); ++finished; });
        io.post([&] { ++queued; });
        std::thread worker([&] { io.run(); });
        const bool ready = started.wait_for(5s) == std::future_status::ready;
        io.stop();
        release.set_value();
        worker.join();
        io.run(); // A stopped queue cannot be restarted.
        check(ready && finished == 1 && queued == 0, "stop drained the queue or interrupted running work");
    }
    std::cout << "PASS: " << checks << " portable IO queue and header checks\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
