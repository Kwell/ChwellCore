// Standalone Epoll Reactor Stress Test
#include <iostream>
#include <iomanip>
#include <mutex>
// Server + Client in one process, using raw epoll
// Usage: ./stress_all [concurrency] [msg_size] [duration_s]
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <string>

struct Stats {
    std::atomic<uint64_t> total_sent{0};
    std::atomic<uint64_t> total_recv{0};
    std::atomic<uint64_t> total_recv_bytes{0};
    std::atomic<uint64_t> connect_errors{0};
};

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_server(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    set_nonblocking(fd);
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(fd, 4096);
    return fd;
}

static int connect_to_server(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    set_nonblocking(fd);
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    // EINPROGRESS is expected for non-blocking connect
    return fd;
}

int main(int argc, char* argv[]) {
    int concurrency = 100;
    int msg_size = 1024;
    int duration_s = 30;
    uint16_t port = 19876;
    
    if (argc > 1) concurrency = std::stoi(argv[1]);
    if (argc > 2) msg_size = std::stoi(argv[2]);
    if (argc > 3) duration_s = std::stoi(argv[3]);
    
    // Generate message payload
    std::vector<char> message(msg_size);
    for (int i = 0; i < msg_size; i++) message[i] = 'A' + (i % 26);
    
    Stats stats;
    
    // ========== SERVER THREAD ==========
    std::atomic<bool> server_running(true);
    int server_fd = create_server(port);
    
    // Accept thread
    std::vector<int> server_client_fds;
    std::mutex server_fds_mutex;
    
    auto accept_thread = std::thread([&]() {
        int epfd = epoll_create1(0);
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = server_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);
        
        struct epoll_event events[256];
        char buf[8192];
        
        while (server_running.load()) {
            int n = epoll_wait(epfd, events, 256, 100);
            for (int i = 0; i < n; i++) {
                if (events[i].data.fd == server_fd) {
                    // Accept new connections
                    while (true) {
                        int cfd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK);
                        if (cfd < 0) break;
                        
                        struct epoll_event cev{};
                        cev.events = EPOLLIN;
                        cev.data.fd = cfd;
                        epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                        
                        std::lock_guard<std::mutex> lock(server_fds_mutex);
                        server_client_fds.push_back(cfd);
                    }
                } else {
                    // Read from client, echo back
                    int cfd = events[i].data.fd;
                    ssize_t nread = recv(cfd, buf, sizeof(buf), 0);
                    if (nread <= 0) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, NULL);
                        close(cfd);
                        continue;
                    }
                    // Echo back
                    size_t offset = 0;
                    while (offset < (size_t)nread) {
                        ssize_t nw = send(cfd, buf + offset, nread - offset, 0);
                        if (nw <= 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                // Retry
                                std::this_thread::sleep_for(std::chrono::microseconds(10));
                                continue;
                            }
                            break;
                        }
                        offset += nw;
                    }
                }
            }
        }
        
        close(epfd);
        for (int fd : server_client_fds) close(fd);
        close(server_fd);
    });
    
    // Wait for server to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // ========== CLIENT CONNECTIONS ==========
    std::vector<int> client_fds;
    
    std::cout << "Connecting " << concurrency << " clients to port " << port << "...\n";
    auto conn_start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < concurrency; ++i) {
        int fd = connect_to_server(port);
        if (fd >= 0) {
            client_fds.push_back(fd);
        } else {
            stats.connect_errors++;
        }
    }
    
    // Wait for all connections to establish
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    auto conn_end = std::chrono::steady_clock::now();
    double conn_time_ms = std::chrono::duration<double, std::milli>(conn_end - conn_start).count();
    
    std::cout << "Connected: " << client_fds.size() << "/" << concurrency 
              << " in " << std::fixed << std::setprecision(1) << conn_time_ms << "ms\n";
    
    if (client_fds.empty()) {
        std::cerr << "ERROR: No connections established!\n";
        server_running.store(false);
        accept_thread.join();
        return 1;
    }
    
    // ========== SEND/RECV LOOP ==========
    std::atomic<bool> test_running(true);
    
    // Send thread
    std::thread send_thread([&]() {
        while (test_running.load()) {
            for (int fd : client_fds) {
                ssize_t n = send(fd, message.data(), msg_size, MSG_NOSIGNAL);
                if (n > 0) stats.total_sent.fetch_add(1);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    
    // Recv thread
    std::thread recv_thread([&]() {
        char buf[8192];
        while (test_running.load()) {
            for (int fd : client_fds) {
                ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n > 0) {
                    stats.total_recv.fetch_add(1);
                    stats.total_recv_bytes.fetch_add(n);
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    
    // Monitor thread
    std::atomic<int> remaining(duration_s);
    uint64_t last_sent = 0, last_recv = 0;
    auto last_check = std::chrono::steady_clock::now();
    
    std::thread monitor_thread([&]() {
        while (remaining.load() > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            int left = remaining.fetch_sub(1);
            
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - last_check).count();
            
            uint64_t cur_sent = stats.total_sent.load();
            uint64_t cur_recv = stats.total_recv.load();
            
            double qps_s = (cur_sent - last_sent) / elapsed;
            double qps_r = (cur_recv - last_recv) / elapsed;
            double bw_mbps = ((cur_recv - last_recv) * msg_size) / elapsed / 1024.0 / 1024.0;
            
            printf("[%2ds remaining] Sent: %8lu  Recv: %8lu  |  %10.0f QPS send  %10.0f QPS recv  |  %6.2f MB/s\n",
                   left, cur_sent, cur_recv, qps_s, qps_r, bw_mbps);
            fflush(stdout);
            
            last_sent = cur_sent;
            last_recv = cur_recv;
            last_check = now;
        }
        test_running.store(false);
    });
    
    auto bench_start = std::chrono::steady_clock::now();
    
    // Wait for test to finish
    while (remaining.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    test_running.store(false);
    send_thread.join();
    recv_thread.join();
    monitor_thread.join();
    
    auto bench_end = std::chrono::steady_clock::now();
    double total_s = std::chrono::duration<double>(bench_end - bench_start).count();
    
    // Cleanup
    for (int fd : client_fds) close(fd);
    server_running.store(false);
    accept_thread.join();
    
    // Final stats
    uint64_t final_sent = stats.total_sent.load();
    uint64_t final_recv = stats.total_recv.load();
    uint64_t final_bytes = stats.total_recv_bytes.load();
    
    double avg_qps_sent = final_sent / total_s;
    double avg_qps_recv = final_recv / total_s;
    double avg_bw = final_bytes / total_s / 1024.0 / 1024.0;
    double avg_latency_us = (total_s * 1000000.0 * concurrency) / (final_recv > 0 ? final_recv : 1);
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║         STRESS TEST RESULTS (Epoll Echo Server)           ║\n");
    printf("╠════════════════════════════════════════════════════════════╣\n");
    printf("║ Concurrency:       %8d connections                  ║\n", (int)client_fds.size());
    printf("║ Message Size:      %8d bytes                        ║\n", msg_size);
    printf("║ Duration:          %8.2f seconds                      ║\n", total_s);
    printf("║ Connection Time:   %8.1f ms                           ║\n", conn_time_ms);
    printf("╠════════════════════════════════════════════════════════════╣\n");
    printf("║ Total Sent:        %8lu messages                      ║\n", final_sent);
    printf("║ Total Recv:        %8lu messages                      ║\n", final_recv);
    printf("║ Total Recv Bytes:  %8lu MB                            ║\n", final_bytes / (1024*1024));
    printf("╠════════════════════════════════════════════════════════════╣\n");
    printf("║ Avg Send QPS:      %12.0f                              ║\n", avg_qps_sent);
    printf("║ Avg Recv QPS:      %12.0f                              ║\n", avg_qps_recv);
    printf("║ Avg Bandwidth:     %12.2f MB/s                         ║\n", avg_bw);
    printf("║ Avg Latency:       %12.2f μs                           ║\n", avg_latency_us);
    printf("║ Throughput/Conn:   %12.0f msg/s                        ║\n", avg_qps_recv / client_fds.size());
    printf("╚════════════════════════════════════════════════════════════╝\n");
    
    return 0;
}
