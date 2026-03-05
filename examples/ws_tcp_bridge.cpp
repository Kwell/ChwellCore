// WebSocket <-> TCP 桥接：浏览器通过 WS 连接本服务，本服务将数据转发到游戏网关 TCP。
// 需 OpenSSL（libssl）用于 WebSocket 握手的 SHA1。

#include <cstdlib>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <map>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

namespace {

static const char* WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const int WS_PORT = 9080;
// 网关地址：可通过环境变量 GATEWAY_HOST、GATEWAY_PORT 覆盖，便于前后端分离部署
static std::string gateway_host() {
    const char* p = std::getenv("GATEWAY_HOST");
    return p && p[0] ? p : "127.0.0.1";
}
static int gateway_port() {
    const char* p = std::getenv("GATEWAY_PORT");
    if (p && p[0]) { int n = std::atoi(p); if (n > 0) return n; }
    return 9001;
}

std::string base64_encode(const unsigned char* data, size_t len) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64, mem);
    BIO_write(b64, data, static_cast<int>(len));
    BIO_flush(b64);
    BUF_MEM* ptr;
    BIO_get_mem_ptr(b64, &ptr);
    std::string out(ptr->data, ptr->length);
    BIO_free_all(b64);
    return out;
}

std::string ws_accept_key(const std::string& key) {
    std::string s = key + WS_MAGIC;
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(s.data()), s.size(), hash);
    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

bool do_ws_handshake(int client_fd, std::string& key_out) {
    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return false;
    buf[n] = '\0';
    std::string headers(buf);
    key_out.clear();
    size_t pos = 0;
    for (;;) {
        size_t end = headers.find("\r\n", pos);
        if (end == std::string::npos) break;
        std::string line = headers.substr(pos, end - pos);
        pos = end + 2;
        if (line.empty()) break;
        if (line.find("Sec-WebSocket-Key:") == 0) {
            size_t start = line.find(':') + 1;
            while (start < line.size() && line[start] == ' ') start++;
            key_out = line.substr(start);
            while (!key_out.empty() && key_out.back() == '\r') key_out.pop_back();
            break;
        }
    }
    if (key_out.empty()) return false;
    std::string accept = ws_accept_key(key_out);
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    ssize_t sent = send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(response.size());
}

// 从 WebSocket 帧读取 payload（单帧，二进制）
bool ws_read_frame(int fd, std::vector<char>& out) {
    unsigned char hdr[2];
    if (recv(fd, hdr, 2, MSG_WAITALL) != 2) return false;
    unsigned char op = hdr[0] & 0x0f;
    bool mask = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7f;
    if (payload_len == 126) {
        unsigned char ext[2];
        if (recv(fd, ext, 2, MSG_WAITALL) != 2) return false;
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        unsigned char ext[8];
        if (recv(fd, ext, 8, MSG_WAITALL) != 8) return false;
        payload_len = 0;
        for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
    }
    if (op == 8) return false; // close
    if (payload_len > 4 * 1024 * 1024) return false;
    unsigned char mask_key[4];
    if (mask) {
        if (recv(fd, mask_key, 4, MSG_WAITALL) != 4) return false;
    }
    out.resize(static_cast<size_t>(payload_len));
    if (payload_len == 0) return true;
    ssize_t got = recv(fd, out.data(), out.size(), MSG_WAITALL);
    if (got != static_cast<ssize_t>(payload_len)) return false;
    if (mask) {
        for (size_t i = 0; i < out.size(); i++)
            out[i] = out[i] ^ mask_key[i % 4];
    }
    return true;
}

// 发送 WebSocket 二进制帧
bool ws_write_frame(int fd, const char* data, size_t len) {
    std::vector<unsigned char> frame;
    frame.push_back(0x82); // binary
    if (len < 126) {
        frame.push_back(static_cast<unsigned char>(len));
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<unsigned char>(len >> 8));
        frame.push_back(static_cast<unsigned char>(len & 0xff));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--)
            frame.push_back(static_cast<unsigned char>((len >> (i * 8)) & 0xff));
    }
    frame.insert(frame.end(), data, data + len);
    ssize_t sent = send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(frame.size());
}

int connect_to_gateway() {
    std::string host = gateway_host();
    int port = gateway_port();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void bridge_loop(int client_fd) {
    std::string key;
    if (!do_ws_handshake(client_fd, key)) {
        close(client_fd);
        return;
    }
    int gate_fd = connect_to_gateway();
    if (gate_fd < 0) {
        std::cerr << "Bridge: cannot connect to gateway " << gateway_host() << ":" << gateway_port() << std::endl;
        close(client_fd);
        return;
    }
    std::atomic<bool> done{false};
    auto ws_to_tcp = [&]() {
        std::vector<char> buf;
        while (!done) {
            if (!ws_read_frame(client_fd, buf)) break;
            if (buf.empty()) continue;
            ssize_t n = send(gate_fd, buf.data(), buf.size(), MSG_NOSIGNAL);
            if (n != static_cast<ssize_t>(buf.size())) break;
        }
        done = true;
        shutdown(gate_fd, SHUT_WR);
        shutdown(client_fd, SHUT_RD);
    };
    auto tcp_to_ws = [&]() {
        char buf[4096];
        while (!done) {
            ssize_t n = recv(gate_fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (!ws_write_frame(client_fd, buf, static_cast<size_t>(n))) break;
        }
        done = true;
        shutdown(client_fd, SHUT_WR);
        shutdown(gate_fd, SHUT_RD);
    };
    std::thread t1(ws_to_tcp);
    std::thread t2(tcp_to_ws);
    t1.join();
    t2.join();
    close(gate_fd);
    close(client_fd);
}

static volatile sig_atomic_t g_stop = 0;
} // namespace

int main(int argc, char* argv[]) {
    int ws_port = WS_PORT;
    if (argc >= 2) ws_port = std::atoi(argv[1]);
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket failed" << std::endl;
        return 1;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(ws_port));
    if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind failed" << std::endl;
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 8) < 0) {
        std::cerr << "listen failed" << std::endl;
        close(listen_fd);
        return 1;
    }
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::cout << "WS-TCP Bridge: ws://0.0.0.0:" << ws_port << " -> " << gateway_host() << ":" << gateway_port() << std::endl;
    while (!g_stop) {
        struct pollfd pfd = { listen_fd, POLLIN, 0 };
        int r = poll(&pfd, 1, 1000);
        if (r <= 0) continue;
        struct sockaddr_in peer = {};
        socklen_t len = sizeof(peer);
        int client = accept(listen_fd, reinterpret_cast<struct sockaddr*>(&peer), &len);
        if (client < 0) continue;
        std::thread(bridge_loop, client).detach();
    }
    close(listen_fd);
    return 0;
}
