#include "chwell/net/udp_server.h"
#include "chwell/core/logger.h"
#include <cstring>

namespace chwell {
namespace net {

UdpServer::UdpServer(IoService& io_service, unsigned short port)
    : io_service_(io_service), buffer_(65536) {
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        core::Logger::instance().error("UdpServer bind failed");
        close(fd_);
        fd_ = -1;
    }
}

void UdpServer::start_receive() {
    if (fd_ < 0) return;
    stopped_ = false;
    recv_thread_ = std::thread([this]() { recv_loop(); });
}

void UdpServer::stop() {
    stopped_ = true;
    if (fd_ >= 0) {
        shutdown(fd_, SHUT_RDWR);
    }
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void UdpServer::recv_loop() {
    sockaddr_in remote_addr{};
    socklen_t addr_len = sizeof(remote_addr);

    while (!stopped_ && fd_ >= 0) {
        ssize_t n = recvfrom(fd_, buffer_.data(), buffer_.size(), 0,
                            reinterpret_cast<sockaddr*>(&remote_addr), &addr_len);
        if (n <= 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }

        UdpEndpoint remote;
        remote.addr_ = remote_addr;

        std::vector<char> data(buffer_.begin(), buffer_.begin() + n);
        if (message_cb_) {
            io_service_.post([this, data, remote]() {
                message_cb_(data, remote);
            });
        }
    }
}

void UdpServer::send_to(const std::vector<char>& data, const UdpEndpoint& remote) {
    if (fd_ < 0 || data.empty()) return;
    ssize_t n = ::sendto(fd_, data.data(), data.size(), 0,
                         reinterpret_cast<const sockaddr*>(&remote.addr_),
                         sizeof(remote.addr_));
    if (n < 0) {
        core::Logger::instance().warn("UDP send failed: " + std::string(strerror(errno)));
    }
}

} // namespace net
} // namespace chwell
