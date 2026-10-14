#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <string>

namespace chwell { namespace net {

// Native error values are interpreted by the selected backend.
struct ErrorCode {
    int value_{0};
    ErrorCode() = default;
    explicit ErrorCode(int value) : value_(value) {}
    bool operator!() const { return value_ == 0; }
    operator bool() const { return value_ != 0; }
    std::string message() const;
};
namespace errc {
    constexpr int operation_aborted = ECONNABORTED;
}

// Blocking socket declarations. The current backend uses POSIX int descriptors;
// a platform-neutral header does not imply a Windows socket implementation.
class TcpSocket {
public:
    TcpSocket() : fd_(-1) {}
    explicit TcpSocket(int fd) : fd_(fd) {}
    ~TcpSocket();
    TcpSocket(TcpSocket&& other) noexcept : fd_(other.fd_.exchange(-1)) {}
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    int native_handle() const { return fd_.load(std::memory_order_acquire); }
    bool is_open() const { return native_handle() >= 0; }
    // Close only after concurrent reads/writes/shutdown calls have finished.
    // shutdown may wake a blocking reader; descriptor ownership stays here.
    void close(ErrorCode& ec);
    void shutdown(int how, ErrorCode& ec);
    std::ptrdiff_t read(void* buf, std::size_t len);
    std::ptrdiff_t write(const void* buf, std::size_t len);
private:
    void close_fd();
    std::atomic<int> fd_;
};

class TcpAcceptor {
public:
    TcpAcceptor(unsigned short port);
    ~TcpAcceptor();
    TcpAcceptor(const TcpAcceptor&) = delete;
    TcpAcceptor& operator=(const TcpAcceptor&) = delete;
    int listen_fd() const { return listen_fd_; }
    TcpSocket accept(ErrorCode& ec);
private:
    int listen_fd_{-1};
};

} } // namespace chwell::net
