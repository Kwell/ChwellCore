// Framework end-to-end benchmark: real Service + ProtocolRouter echo path.
//
// Closed-loop client: each connection keeps at most `inflight` unanswered
// requests, so measured RTT is true round-trip (no open-loop flood).
// Client uses one epoll loop per thread (fixes the old single-recv bottleneck).
//
// Usage: ./framework_bench [conns] [inflight] [msg_size] [duration_s] [reactor_threads] [logic_workers]
// logic_workers=0 means same as reactor_threads.
// Examples:
//   ./framework_bench 200 1 1024 10 2 1     # single logic worker (baseline)
//   ./framework_bench 200 1 1024 10 2 4     # sharded logic workers
//   ./framework_bench 200 16 1024 10 2 4    # throughput

#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/protocol/message.h"
#include "chwell/protocol/parser.h"
#include "chwell/net/posix_io.h"
#include "chwell/core/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace chwell;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::uint16_t kCmdEcho = 0x0001;
constexpr std::uint16_t kCmdEchoResp = 0x0002;
constexpr std::uint16_t kPort = 19890;

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct ConnState {
    int fd = -1;
    bool connected = false;
    protocol::Parser parser;
    std::deque<Clock::time_point> pending;   // FIFO send times (TCP preserves order)
    std::vector<char> outbuf;
    std::size_t out_off = 0;
};

struct ThreadStats {
    std::vector<double> rtt_ms;   // reserve ahead to avoid mid-run realloc
    std::uint64_t sent = 0;
    std::uint64_t recvd = 0;
    std::uint64_t recv_bytes = 0;
    std::uint64_t connect_errors = 0;
};

void run_client_thread(int thread_idx,
                       int conn_count,
                       int inflight,
                       int msg_size,
                       int duration_s,
                       ThreadStats& stats) {
    std::vector<char> payload(msg_size, 'x');
    auto frame = protocol::serialize(protocol::Message(kCmdEcho, payload));
    const std::size_t frame_len = frame.size();

    int epfd = epoll_create1(0);
    std::vector<ConnState> conns(conn_count);

    for (int i = 0; i < conn_count; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { ++stats.connect_errors; continue; }
        set_nonblocking(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPort);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc < 0 && errno != EINPROGRESS) {
            ::close(fd);
            ++stats.connect_errors;
            continue;
        }
        conns[i].fd = fd;
        epoll_event ev{};
        ev.events = EPOLLOUT | EPOLLIN | EPOLLET;
        ev.data.u32 = static_cast<uint32_t>(i);
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }

    stats.rtt_ms.reserve(static_cast<std::size_t>(duration_s) * 200000);

    auto deadline = Clock::now() + std::chrono::seconds(duration_s);
    std::vector<epoll_event> events(256);
    char rbuf[65536];

    auto try_send = [&](ConnState& c) {
        while (c.connected &&
               static_cast<int>(c.pending.size()) < inflight) {
            // write from outbuf first
            if (c.out_off < c.outbuf.size()) break;
            c.outbuf.assign(frame.begin(), frame.end());
            c.out_off = 0;
            // flush immediately
            ssize_t n = ::send(c.fd, c.outbuf.data() + c.out_off,
                               c.outbuf.size() - c.out_off, MSG_NOSIGNAL);
            if (n > 0) {
                c.out_off += static_cast<std::size_t>(n);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                return;
            }
            if (c.out_off >= c.outbuf.size()) {
                c.outbuf.clear();
                c.out_off = 0;
                c.pending.push_back(Clock::now());
                ++stats.sent;
            }
        }
        // flush leftover
        while (c.connected && c.out_off < c.outbuf.size()) {
            ssize_t n = ::send(c.fd, c.outbuf.data() + c.out_off,
                               c.outbuf.size() - c.out_off, MSG_NOSIGNAL);
            if (n > 0) {
                c.out_off += static_cast<std::size_t>(n);
            } else {
                break;
            }
        }
        if (c.out_off >= c.outbuf.size()) {
            c.outbuf.clear();
            c.out_off = 0;
        }
    };

    while (Clock::now() < deadline) {
        int n = epoll_wait(epfd, events.data(), static_cast<int>(events.size()), 200);
        for (int i = 0; i < n; ++i) {
            uint32_t idx = events[i].data.u32;
            if (idx >= conns.size()) continue;
            ConnState& c = conns[idx];
            if (c.fd < 0) continue;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                ::close(c.fd);
                c.fd = -1;
                continue;
            }

            if ((events[i].events & EPOLLOUT) && !c.connected) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err != 0) {
                    ::close(c.fd);
                    c.fd = -1;
                    ++stats.connect_errors;
                    continue;
                }
                c.connected = true;
            }

            if ((events[i].events & EPOLLOUT) && c.connected) {
                try_send(c);
            }

            if (events[i].events & EPOLLIN) {
                ssize_t r = ::recv(c.fd, rbuf, sizeof(rbuf), MSG_DONTWAIT);
                if (r > 0) {
                    stats.recv_bytes += static_cast<std::uint64_t>(r);
                    auto msgs = c.parser.feed(std::string_view(rbuf, static_cast<std::size_t>(r)));
                    for (auto& m : msgs) {
                        ++stats.recvd;
                        if (!c.pending.empty()) {
                            double ms = std::chrono::duration<double, std::milli>(
                                Clock::now() - c.pending.front()).count();
                            c.pending.pop_front();
                            stats.rtt_ms.push_back(ms);
                        }
                    }
                    // more to read in edge mode
                    while (true) {
                        r = ::recv(c.fd, rbuf, sizeof(rbuf), MSG_DONTWAIT);
                        if (r <= 0) break;
                        stats.recv_bytes += static_cast<std::uint64_t>(r);
                        auto more = c.parser.feed(std::string_view(rbuf, static_cast<std::size_t>(r)));
                        for (auto& m : more) {
                            ++stats.recvd;
                            if (!c.pending.empty()) {
                                double ms = std::chrono::duration<double, std::milli>(
                                    Clock::now() - c.pending.front()).count();
                                c.pending.pop_front();
                                stats.rtt_ms.push_back(ms);
                            }
                        }
                    }
                }
            }
        }

        // idle: push more sends even if no EPOLLOUT (level-less ET needs retry)
        for (auto& c : conns) {
            if (c.fd >= 0) try_send(c);
        }
    }

    for (auto& c : conns) {
        if (c.fd >= 0) ::close(c.fd);
    }
    ::close(epfd);
}

double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    std::size_t idx = static_cast<std::size_t>(p * (v.size() - 1));
    return v[idx];
}

}  // namespace

int main(int argc, char* argv[]) {
    int conns = 200;
    int inflight = 1;
    int msg_size = 1024;
    int duration_s = 10;
    int reactors = 2;
    int logic_workers = 0;
    if (argc > 1) conns = std::stoi(argv[1]);
    if (argc > 2) inflight = std::stoi(argv[2]);
    if (argc > 3) msg_size = std::stoi(argv[3]);
    if (argc > 4) duration_s = std::stoi(argv[4]);
    if (argc > 5) reactors = std::stoi(argv[5]);
    if (argc > 6) logic_workers = std::stoi(argv[6]);
    if (msg_size < 1) msg_size = 1;
    if (conns < 1) conns = 1;
    if (inflight < 1) inflight = 1;

    // ---- real framework server ----
    service::Service svc(kPort, /*worker_threads=*/4, /*use_epoll=*/true, reactors, logic_workers);
    auto* router = svc.add_component<service::ProtocolRouterComponent>();
    router->register_handler(kCmdEcho,
        [](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
            service::ProtocolRouterComponent::send_message(
                conn, protocol::Message(kCmdEchoResp, msg.body));
        });
    svc.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // ---- closed-loop client threads ----
    unsigned hc = std::thread::hardware_concurrency();
    int client_threads = std::max(2, static_cast<int>(hc > 0 ? std::min(hc, 8u) : 4u));
    int per = conns / client_threads;
    int extra = conns % client_threads;

    std::vector<ThreadStats> stats(client_threads);
    std::vector<std::thread> pool;
    auto t0 = Clock::now();
    for (int t = 0; t < client_threads; ++t) {
        int n = per + (t < extra ? 1 : 0);
        if (n <= 0) continue;
        pool.emplace_back(run_client_thread, t, n, inflight, msg_size, duration_s,
                          std::ref(stats[t]));
    }
    for (auto& th : pool) th.join();
    double wall_s = std::chrono::duration<double>(Clock::now() - t0).count();

    // ---- aggregate ----
    std::vector<double> all_rtt;
    std::uint64_t sent = 0, recvd = 0, rbytes = 0, cerr = 0;
    for (auto& s : stats) {
        all_rtt.insert(all_rtt.end(), s.rtt_ms.begin(), s.rtt_ms.end());
        sent += s.sent;
        recvd += s.recvd;
        rbytes += s.recv_bytes;
        cerr += s.connect_errors;
    }

    std::printf("\n");
    std::printf("================================================================\n");
    std::printf(" Framework E2E Benchmark  (Service + ProtocolRouter echo)\n");
    std::printf("================================================================\n");
    std::printf(" Server            : Service use_epoll=1, workers=4, reactors=%d, logic=%s\n",
                reactors,
                (logic_workers > 0 ? std::to_string(logic_workers).c_str() : "auto"));
    std::printf(" Client            : %d threads, %d conns, inflight=%d\n", client_threads, conns, inflight);
    std::printf(" Message size      : %d bytes (frame ~%zu B)\n", msg_size, (std::size_t)msg_size + 4);
    std::printf(" Duration          : %.2f s\n", wall_s);
    std::printf(" Connect errors    : %llu\n", (unsigned long long)cerr);
    std::printf("----------------------------------------------------------------\n");
    std::printf(" Sent requests     : %llu  (%.0f /s)\n", (unsigned long long)sent, sent / wall_s);
    std::printf(" Recv responses    : %llu  (%.0f /s)\n", (unsigned long long)recvd, recvd / wall_s);
    std::printf(" Goodput           : %.2f MB/s\n", rbytes / wall_s / 1024.0 / 1024.0);
    std::printf(" Samples (RTT)     : %zu\n", all_rtt.size());
    if (!all_rtt.empty()) {
        double sum = 0;
        for (double v : all_rtt) sum += v;
        double avg = sum / all_rtt.size();
        double mn = *std::min_element(all_rtt.begin(), all_rtt.end());
        double mx = *std::max_element(all_rtt.begin(), all_rtt.end());
        std::printf("----------------------------------------------------------------\n");
        std::printf(" RTT avg           : %8.3f ms\n", avg);
        std::printf(" RTT min           : %8.3f ms\n", mn);
        std::printf(" RTT p50           : %8.3f ms\n", percentile(all_rtt, 0.50));
        std::printf(" RTT p90           : %8.3f ms\n", percentile(all_rtt, 0.90));
        std::printf(" RTT p99           : %8.3f ms\n", percentile(all_rtt, 0.99));
        std::printf(" RTT p999          : %8.3f ms\n", percentile(all_rtt, 0.999));
        std::printf(" RTT max           : %8.3f ms\n", mx);
    }
    std::printf("================================================================\n");

    svc.stop();
    return 0;
}
