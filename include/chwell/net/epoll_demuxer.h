#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <atomic>
#include <chrono>
#include <system_error>

#include "chwell/core/logger.h"

namespace chwell {
namespace net {

// 事件类型位掩码
enum class IoEvent : uint32_t {
    Read      = EPOLLIN,        // 可读
    Write     = EPOLLOUT,       // 可写
    Error     = EPOLLERR,       // 错误
    Hangup    = EPOLLHUP,       // 挂起
    RdHangup  = EPOLLRDHUP,     // 对端关闭
    Priority  = EPOLLPRI,       // 紧急数据
    EdgeTrigger = EPOLLET,      // 边缘触发（用户自行设置）
};

inline IoEvent operator|(IoEvent a, IoEvent b) {
    return static_cast<IoEvent>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline IoEvent operator&(IoEvent a, IoEvent b) {
    return static_cast<IoEvent>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool has_event(IoEvent events, IoEvent flag) {
    return (static_cast<uint32_t>(events) & static_cast<uint32_t>(flag)) != 0;
}

// 事件回调：fd + 触发的事件
using EventCallback = std::function<void(int fd, IoEvent events)>;

// fd 上下文
struct FdContext {
    EventCallback callback;
    uint32_t registered_events;  // 注册时的事件（不含 ET）
    bool edge_trigger;
};

// 将 socket 设置为非阻塞
inline bool set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

// 创建非阻塞 socket
inline int create_nonblocking_socket(int domain, int type, int protocol = 0) {
    int fd = ::socket(domain, type, protocol);
    if (fd >= 0) set_nonblocking(fd);
    return fd;
}

// EpollDemuxer — 单线程 epoll 事件循环
class EpollDemuxer {
public:
    explicit EpollDemuxer(int max_events = 1024)
        : epoll_fd_(-1)
        , wake_fd_(-1)
        , max_events_(max_events)
        , stopped_(false) {
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            CHWELL_LOG_ERROR("EpollDemuxer: epoll_create1 failed: " + std::string(std::strerror(errno)));
            return;
        }

        // eventfd 用于线程安全唤醒
        wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd_ < 0) {
            CHWELL_LOG_ERROR("EpollDemuxer: eventfd failed: " + std::string(std::strerror(errno)));
            ::close(epoll_fd_);
            epoll_fd_ = -1;
            return;
        }

        // 注册 wake_fd
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = wake_fd_;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) < 0) {
            CHWELL_LOG_ERROR("EpollDemuxer: epoll_ctl wake_fd failed: " + std::string(std::strerror(errno)));
            ::close(wake_fd_);
            ::close(epoll_fd_);
            wake_fd_ = -1;
            epoll_fd_ = -1;
            return;
        }
    }

    ~EpollDemuxer() {
        stop();
        if (wake_fd_ >= 0) ::close(wake_fd_);
        if (epoll_fd_ >= 0) ::close(epoll_fd_);
    }

    EpollDemuxer(const EpollDemuxer&) = delete;
    EpollDemuxer& operator=(const EpollDemuxer&) = delete;

    bool is_valid() const { return epoll_fd_ >= 0; }
    int native_handle() const { return epoll_fd_; }

    // 注册 fd + 事件 + 回调
    // 默认水平触发（可靠），如需边缘触发请设置 edge_trigger=true
    bool add(int fd, IoEvent events, EventCallback cb, bool edge_trigger = false) {
        if (epoll_fd_ < 0) return false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            fd_contexts_[fd] = {std::move(cb), static_cast<uint32_t>(events), edge_trigger};
        }

        struct epoll_event ev{};
        ev.events = static_cast<uint32_t>(events) | (edge_trigger ? EPOLLET : 0);
        ev.data.fd = fd;
        return ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) >= 0;
    }

    // 修改注册的事件
    bool modify(int fd, IoEvent events) {
        if (epoll_fd_ < 0) return false;

        bool et = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = fd_contexts_.find(fd);
            if (it != fd_contexts_.end()) {
                it->second.registered_events = static_cast<uint32_t>(events);
                et = it->second.edge_trigger;
            }
        }

        struct epoll_event ev{};
        ev.events = static_cast<uint32_t>(events) | (et ? EPOLLET : 0);
        ev.data.fd = fd;
        return ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) >= 0;
    }

    // 修改回调
    bool set_callback(int fd, EventCallback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = fd_contexts_.find(fd);
        if (it == fd_contexts_.end()) return false;
        it->second.callback = std::move(cb);
        return true;
    }

    // 注销 fd
    bool remove(int fd) {
        if (epoll_fd_ < 0) return false;

        bool ok = ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) >= 0;

        std::lock_guard<std::mutex> lock(mutex_);
        fd_contexts_.erase(fd);
        return ok;
    }

    // 事件循环（阻塞，在调用线程运行）
    void run(int timeout_ms = -1) {
        if (epoll_fd_ < 0) {
            CHWELL_LOG_ERROR("EpollDemuxer::run: invalid epoll fd");
            return;
        }

        stopped_ = false;
        std::vector<struct epoll_event> events(max_events_);

        CHWELL_LOG_INFO("EpollDemuxer: event loop starting");

        while (!stopped_) {
            int n = ::epoll_wait(epoll_fd_, events.data(), max_events_, timeout_ms);
            if (n < 0) {
                if (errno == EINTR) continue;
                CHWELL_LOG_ERROR("EpollDemuxer: epoll_wait error: " + std::string(std::strerror(errno)));
                break;
            }

            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                CHWELL_LOG_DEBUG("EpollDemuxer: epoll event fd=" << fd
                                << " events=0x" << std::hex << events[i].events << std::dec);

                // wake_fd 只用于唤醒，不回调
                if (fd == wake_fd_) {
                    uint64_t val;
                    while (::read(wake_fd_, &val, sizeof(val)) > 0) {}
                    continue;
                }

                EventCallback cb;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = fd_contexts_.find(fd);
                    if (it != fd_contexts_.end()) {
                        cb = it->second.callback;
                    }
                }

                if (cb) {
                    cb(fd, static_cast<IoEvent>(events[i].events));
                }
            }
        }

        CHWELL_LOG_INFO("EpollDemuxer: event loop stopped");
    }

    // 从其他线程安全唤醒事件循环
    bool wake() {
        if (wake_fd_ < 0) return false;
        uint64_t val = 1;
        return ::write(wake_fd_, &val, sizeof(val)) == sizeof(val);
    }

    // 停止事件循环
    void stop() {
        if (!stopped_) {
            stopped_ = true;
            wake();
        }
    }

private:
    int epoll_fd_;
    int wake_fd_;
    int max_events_;
    std::atomic<bool> stopped_;

    std::mutex mutex_;
    std::unordered_map<int, FdContext> fd_contexts_;
};

} // namespace net
} // namespace chwell
