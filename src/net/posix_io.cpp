#include "chwell/net/posix_io.h"
#include <thread>

namespace chwell {
namespace net {

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
    if (wake_fd_ >= 0) {
        close(wake_fd_);
        wake_fd_ = -1;
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

// IoService
void IoService::run() {
    while (!stopped_) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
            if (stopped_) break;
            if (queue_.empty()) continue;
            task = std::move(queue_.front());
            queue_.pop();
        }
        if (task) task();
    }
}

void IoService::stop() {
    stopped_ = true;
    cv_.notify_all();
}

void IoService::post(std::function<void()> f) {
    if (!f) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(f));
    }
    cv_.notify_one();
}

} // namespace net
} // namespace chwell
