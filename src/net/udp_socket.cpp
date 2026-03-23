#include "chwell/net/udp_socket.h"
#include "chwell/core/logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

#include <system_error>

namespace chwell {
namespace net {

UdpSocket::UdpSocket(int fd)
    : fd_(fd), local_port_(0) {
}

UdpSocket::~UdpSocket() {
    close();
}

void UdpSocket::send_to(const std::string& host, uint16_t port, const std::string& data) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        CHWELL_LOG_ERROR("Invalid host address: " + host);
        return;
    }

    ssize_t sent = ::sendto(fd_, data.c_str(), data.size(), 0,
                           reinterpret_cast<struct sockaddr*>(&addr),
                           sizeof(addr));

    if (sent < 0) {
        CHWELL_LOG_ERROR("UDP sendto failed: " + std::string(std::strerror(errno)));
    } else if (static_cast<size_t>(sent) != data.size()) {
        CHWELL_LOG_WARN("UDP partial send: " + std::to_string(sent) +
                          " / " + std::to_string(data.size()));
    }
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;

        if (close_cb_) {
            // weak_from_this().lock() 在正常关闭时返回有效指针，
            // 在析构期调用时返回 null（此时引用计数已归零）
            close_cb_(weak_from_this().lock());
        }

        CHWELL_LOG_INFO("UDP socket closed");
    }
}

UdpSocketPtr create_udp_socket(const std::string& host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        CHWELL_LOG_ERROR("Failed to create UDP socket: " +
                          std::string(std::strerror(errno)));
        return nullptr;
    }

    auto socket = std::make_shared<UdpSocket>(fd);

    // 若调用方指定了端口，则立即绑定；port==0 时仅创建不绑定
    if (port != 0) {
        if (!bind_udp_port(socket, host, port)) {
            return nullptr;
        }
    }

    return socket;
}

bool bind_udp_port(UdpSocketPtr& socket, const std::string& host, uint16_t port) {
    if (!socket) {
        CHWELL_LOG_ERROR("Cannot bind null socket");
        return false;
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // 空字符串或 "0.0.0.0" 时监听全部网卡，否则按指定地址绑定
    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            CHWELL_LOG_ERROR("Invalid bind address: " + host);
            return false;
        }
    }

    if (bind(socket->native_handle(), reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        CHWELL_LOG_ERROR("Failed to bind UDP port " + std::to_string(port) + ": " +
                          std::string(std::strerror(errno)));
        return false;
    }

    // 回填实际绑定的地址和端口（支持 port=0 时系统分配）
    struct sockaddr_in bound_addr;
    socklen_t addr_len = sizeof(bound_addr);
    if (getsockname(socket->native_handle(),
                   reinterpret_cast<struct sockaddr*>(&bound_addr), &addr_len) < 0) {
        CHWELL_LOG_WARN("Failed to get bound port");
        return false;
    }

    socket->local_port_ = ntohs(bound_addr.sin_port);
    socket->local_addr_ = host.empty() ? "0.0.0.0" : host;

    CHWELL_LOG_INFO("UDP socket bound to " + socket->local_addr_ +
                    ":" + std::to_string(socket->local_port_));
    return true;
}

} // namespace net
} // namespace chwell
