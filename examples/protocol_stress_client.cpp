#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

// 协议格式：| cmd (2 bytes) | len (2 bytes) | body (len bytes) |
struct ProtocolMessage {
    uint16_t cmd;
    uint16_t len;
    std::vector<char> body;

    ProtocolMessage(uint16_t c, const std::string& b) : cmd(c), body(b.begin(), b.end()) {
        len = htons(static_cast<uint16_t>(body.size()));
        cmd = htons(cmd);
    }

    std::vector<char> serialize() const {
        std::vector<char> data;
        data.resize(4 + body.size());
        std::memcpy(&data[0], &cmd, 2);
        std::memcpy(&data[2], &len, 2);
        std::memcpy(&data[4], body.data(), body.size());
        return data;
    }
};

// 命令号
const uint16_t CMD_ECHO = 1;
const uint16_t CMD_CHAT = 2;
const uint16_t CMD_HEARTBEAT = 3;
const uint16_t CMD_LOGIN = 10;

// 性能统计
struct Stats {
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_bytes{0};
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;

    void start() {
        start_time = std::chrono::steady_clock::now();
    }

    void stop() {
        end_time = std::chrono::steady_clock::now();
    }

    double elapsed_seconds() const {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        return duration.count() / 1000.0;
    }

    double ops_per_second() const {
        double elapsed = elapsed_seconds();
        if (elapsed <= 0) return 0;
        return total_requests.load() / elapsed;
    }

    double mb_per_second() const {
        double elapsed = elapsed_seconds();
        if (elapsed <= 0) return 0;
        return (total_bytes.load() / 1024.0 / 1024.0) / elapsed;
    }
};

// 客户端worker - 使用连接池
void client_worker(int worker_id, const std::string& host, int port,
                   int requests_per_worker, int message_size, Stats& stats) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host.c_str());
        if (!he) {
            std::cerr << "Worker " << worker_id << ": DNS resolution failed" << std::endl;
            return;
        }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }

    // 创建连接
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Worker " << worker_id << ": socket failed" << std::endl;
        return;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        std::cerr << "Worker " << worker_id << ": connect failed" << std::endl;
        return;
    }

    // 设置超时
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::cout << "Worker " << worker_id << " connected to server" << std::endl;

    // 发送所有请求
    for (int i = 0; i < requests_per_worker; ++i) {
        // 构造消息
        std::string body(message_size, 'x');
        body += "_worker" + std::to_string(worker_id) + "_req" + std::to_string(i);
        ProtocolMessage msg(CMD_ECHO, body);
        auto data = msg.serialize();

        // 发送
        ssize_t sent = send(sock, data.data(), data.size(), 0);
        if (sent != (ssize_t)data.size()) {
            stats.total_errors++;
            continue;
        }

        stats.total_bytes += data.size();

        // 接收响应
        char header[4];
        ssize_t received = recv(sock, header, 4, MSG_WAITALL);
        if (received != 4) {
            stats.total_errors++;
            continue;
        }

        uint16_t resp_len = ntohs(*(uint16_t*)(header + 2));
        std::vector<char> resp_body(resp_len);
        received = recv(sock, resp_body.data(), resp_len, MSG_WAITALL);
        if (received != (ssize_t)resp_len) {
            stats.total_errors++;
            continue;
        }

        stats.total_requests++;
        stats.total_bytes += 4 + resp_len;
    }

    close(sock);
    std::cout << "Worker " << worker_id << " completed " << requests_per_worker << " requests" << std::endl;
}

int main(int argc, char* argv[]) {
    // 默认参数
    std::string host = "127.0.0.1";
    int port = 9000;
    int workers = 10;
    int requests_per_worker = 1000;
    int message_size = 100;

    // 解析参数
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = std::atoi(argv[2]);
    if (argc >= 4) workers = std::atoi(argv[3]);
    if (argc >= 5) requests_per_worker = std::atoi(argv[4]);
    if (argc >= 6) message_size = std::atoi(argv[5]);

    std::cout << "==========================================\n";
    std::cout << "Protocol Stress Test Client\n";
    std::cout << "==========================================\n";
    std::cout << "Host: " << host << ":" << port << "\n";
    std::cout << "Workers: " << workers << "\n";
    std::cout << "Requests per worker: " << requests_per_worker << "\n";
    std::cout << "Message size: " << message_size << " bytes\n";
    std::cout << "Total requests: " << (workers * requests_per_worker) << "\n";
    std::cout << "==========================================\n\n";

    Stats stats;

    std::vector<std::thread> threads;
    threads.reserve(workers);

    // 启动worker线程
    stats.start();
    for (int i = 0; i < workers; ++i) {
        threads.emplace_back(client_worker, i, host, port, requests_per_worker, message_size, std::ref(stats));
    }

    // 等待所有worker完成
    for (auto& t : threads) {
        t.join();
    }
    stats.stop();

    // 打印统计结果
    std::cout << "\n==========================================\n";
    std::cout << "Test Results\n";
    std::cout << "==========================================\n";
    std::cout << "Total requests: " << stats.total_requests.load() << "\n";
    std::cout << "Total errors: " << stats.total_errors.load() << "\n";
    std::cout << "Success rate: "
              << (stats.total_requests.load() * 100.0 / (stats.total_requests.load() + stats.total_errors.load()))
              << "%\n";
    std::cout << "Total bytes: " << stats.total_bytes.load() << " ("
              << (stats.total_bytes.load() / 1024.0 / 1024.0) << " MB)\n";
    std::cout << "Elapsed time: " << stats.elapsed_seconds() << " seconds\n";
    std::cout << "Requests/sec: " << std::fixed << std::setprecision(2) << stats.ops_per_second() << "\n";
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << stats.mb_per_second() << " MB/s\n";
    std::cout << "==========================================\n";

    return 0;
}
