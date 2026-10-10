#include "chwell/cluster/tcp_rpc_transport.h"
#include "chwell/protocol/message.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace chwell { namespace cluster {
namespace {
using Deadline = std::chrono::steady_clock::time_point;
bool wait_ready(int fd, short event, Deadline deadline) {
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return false;
        pollfd pending{fd, event, 0};
        const int result = ::poll(&pending, 1, static_cast<int>(remaining));
        if (result < 0 && errno == EINTR) continue;
        return result > 0 && (pending.revents & event);
    }
}
bool transfer(int fd, char* data, std::size_t bytes, bool sending, Deadline deadline) {
    while (bytes) {
        if (!wait_ready(fd, sending ? POLLOUT : POLLIN, deadline)) return false;
        const auto count = sending ? ::send(fd, data, bytes, MSG_NOSIGNAL) : ::recv(fd, data, bytes, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return false;
        data += count;
        bytes -= static_cast<std::size_t>(count);
    }
    return true;
}
} // namespace

TcpRpcTransport::~TcpRpcTransport() {
    if (fd_ >= 0) ::close(fd_);
}

bool TcpRpcTransport::close_failed() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    healthy_ = false;
    return false;
}

bool TcpRpcTransport::call(std::uint16_t cmd, const std::vector<char>& request,
                           std::vector<char>& response, int timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!healthy_ || timeout_ms <= 0 || request.size() > 65531) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    if (fd_ < 0) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(node_.listen_port);
        if (!node_.listen_port || ::inet_pton(AF_INET, node_.listen_addr.c_str(), &address.sin_addr) != 1) return close_failed();
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd_ < 0) return close_failed();
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            if (errno != EINPROGRESS || !wait_ready(fd_, POLLOUT, deadline)) return close_failed();
            int error = 0;
            socklen_t length = sizeof(error);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error) return close_failed();
        }
    }
    const auto request_id = htonl(next_id_++);
    std::vector<char> body(4 + request.size());
    std::memcpy(body.data(), &request_id, 4);
    if (!request.empty()) std::memcpy(body.data() + 4, request.data(), request.size());
    auto frame = protocol::serialize(protocol::Message(cmd, body));
    if (!transfer(fd_, frame.data(), frame.size(), true, deadline)) return close_failed();
    char header[4];
    if (!transfer(fd_, header, sizeof(header), false, deadline)) return close_failed();
    std::uint16_t response_cmd, length;
    std::memcpy(&response_cmd, header, 2);
    std::memcpy(&length, header + 2, 2);
    if (ntohs(response_cmd) != cmd || ntohs(length) < 4) return close_failed();
    std::vector<char> received(ntohs(length));
    if (!transfer(fd_, received.data(), received.size(), false, deadline) ||
        std::memcmp(received.data(), &request_id, 4) != 0) return close_failed();
    response.assign(received.begin() + 4, received.end());
    return true;
}

} } // namespace chwell::cluster
