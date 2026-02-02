#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

namespace chwell {
namespace net {

// 错误码，兼容 asio::error_code 的用法
struct ErrorCode {
    int value_{0};
    ErrorCode() = default;
    explicit ErrorCode(int v) : value_(v) {}
    bool operator!() const { return value_ == 0; }
    operator bool() const { return value_ != 0; }
    std::string message() const {
        if (value_ == 0) return "Success";
        return std::string(strerror(value_));
    }
};

namespace errc {
    constexpr int operation_aborted = ECONNABORTED;
}

// UDP 端点
struct UdpEndpoint {
    sockaddr_in addr_{};
    UdpEndpoint() { memset(&addr_, 0, sizeof(addr_)); }
    UdpEndpoint(const std::string& ip, unsigned short port) {
        memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr);
    }
    std::string address() const {
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
        return std::string(buf);
    }
    unsigned short port() const { return ntohs(addr_.sin_port); }
};

// TCP 套接字封装（阻塞 I/O）
class TcpSocket {
public:
    TcpSocket() : fd_(-1) {}
    explicit TcpSocket(int fd) : fd_(fd) {}
    ~TcpSocket() { close_fd(); }

    TcpSocket(TcpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    TcpSocket& operator=(TcpSocket&& other) noexcept {
        if (this != &other) {
            close_fd();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    int native_handle() const { return fd_; }
    bool is_open() const { return fd_ >= 0; }

    void close(ErrorCode& ec) {
        ec = ErrorCode(0);
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void shutdown(int how, ErrorCode& ec) {
        ec = ErrorCode(0);
        if (fd_ >= 0 && ::shutdown(fd_, how) != 0) {
            ec = ErrorCode(errno);
        }
    }

    // 阻塞读
    ssize_t read(void* buf, std::size_t len) {
        return fd_ >= 0 ? ::read(fd_, buf, len) : -1;
    }

    // 阻塞写
    ssize_t write(const void* buf, std::size_t len) {
        return fd_ >= 0 ? ::write(fd_, buf, len) : -1;
    }

private:
    void close_fd() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_;
    friend class TcpAcceptor;
};

// TCP Acceptor - 阻塞 accept，配合 poll 用于可中断的 accept
class TcpAcceptor {
public:
    TcpAcceptor(unsigned short port);
    ~TcpAcceptor();

    int listen_fd() const { return listen_fd_; }
    TcpSocket accept(ErrorCode& ec);

private:
    int listen_fd_{-1};
    int wake_fd_{-1};  // 用于 stop 时唤醒
};

// IoService - 简化为工作队列 + 停止信号
// 用于兼容现有 API，实际使用线程池处理连接
class IoService {
public:
    IoService() = default;
    ~IoService() = default;

    void run();   // 阻塞直到 stop，可被多线程调用
    void stop();  // 唤醒所有 run()

    void post(std::function<void()> f);

private:
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> queue_;
};

} // namespace net
} // namespace chwell
