#include "chwell/net/socket_io.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>

namespace chwell {
namespace net {

std::string ErrorCode::message() const {
    return value_ == 0 ? "Success" : std::string(std::strerror(value_));
}
TcpSocket::~TcpSocket() { close_fd(); }
TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) { close_fd(); fd_.store(other.fd_.exchange(-1)); }
    return *this;
}
void TcpSocket::close_fd() {
    const int fd = fd_.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) ::close(fd);
}
void TcpSocket::close(ErrorCode& ec) {
    ec = ErrorCode(0);
    close_fd();
}
void TcpSocket::shutdown(int how, ErrorCode& ec) {
    ec = ErrorCode(0);
    const int fd = native_handle();
    if (fd >= 0 && ::shutdown(fd, how) != 0) ec = ErrorCode(errno);
}
std::ptrdiff_t TcpSocket::read(void* buf, std::size_t len) {
    const int fd = native_handle();
    if (fd >= 0) return ::read(fd, buf, len);
    errno = EBADF;
    return -1;
}
std::ptrdiff_t TcpSocket::write(const void* buf, std::size_t len) {
    const int fd = native_handle();
    if (fd >= 0) return ::write(fd, buf, len);
    errno = EBADF;
    return -1;
}

// TcpAcceptor
TcpAcceptor::TcpAcceptor(unsigned short port) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    if (listen(listen_fd_, 128) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

TcpAcceptor::~TcpAcceptor() {
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

TcpSocket TcpAcceptor::accept(ErrorCode& ec) {
    ec = ErrorCode(0);
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
    if (fd < 0) {
        ec = ErrorCode(errno);
        return TcpSocket();
    }
    return TcpSocket(fd);
}

} // namespace net
} // namespace chwell
