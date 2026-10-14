#include "chwell/net/epoll_demuxer.h"
#include "chwell/core/logger.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chwell {
namespace net {
namespace {
constexpr IoEvent known_events = IoEvent::Read | IoEvent::Write | IoEvent::Error |
    IoEvent::Hangup | IoEvent::RdHangup | IoEvent::Priority | IoEvent::EdgeTrigger;

bool valid_events(IoEvent events) {
    if ((static_cast<std::uint32_t>(events) & ~static_cast<std::uint32_t>(known_events)) != 0) {
        errno = EINVAL;
        return false;
    }
    return true;
}

std::uint32_t native_events(IoEvent events, bool edge_trigger) {
    std::uint32_t result = 0;
    if (has_event(events, IoEvent::Read)) result |= EPOLLIN;
    if (has_event(events, IoEvent::Write)) result |= EPOLLOUT;
    if (has_event(events, IoEvent::Error)) result |= EPOLLERR;
    if (has_event(events, IoEvent::Hangup)) result |= EPOLLHUP;
    if (has_event(events, IoEvent::RdHangup)) result |= EPOLLRDHUP;
    if (has_event(events, IoEvent::Priority)) result |= EPOLLPRI;
    if (edge_trigger || has_event(events, IoEvent::EdgeTrigger)) result |= EPOLLET;
    return result;
}

IoEvent portable_events(std::uint32_t events) {
    IoEvent result = IoEvent::None;
    if (events & EPOLLIN) result = result | IoEvent::Read;
    if (events & EPOLLOUT) result = result | IoEvent::Write;
    if (events & EPOLLERR) result = result | IoEvent::Error;
    if (events & EPOLLHUP) result = result | IoEvent::Hangup;
    if (events & EPOLLRDHUP) result = result | IoEvent::RdHangup;
    if (events & EPOLLPRI) result = result | IoEvent::Priority;
    return result;
}
} // namespace

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

int create_nonblocking_socket(int domain, int type, int protocol) {
    const int fd = ::socket(domain, type, protocol);
    if (fd >= 0 && !set_nonblocking(fd)) {
        const int error = errno;
        ::close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

struct EpollDemuxer::Impl {
    struct Context {
        EventCallback callback;
        std::uint64_t token;
        bool edge_trigger;
    };
    int epoll_fd = -1;
    int wake_fd = -1;
    int max_events;
    std::atomic<bool> stopped{false};
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::unordered_map<int, Context> contexts;
    std::unordered_map<std::uint64_t, int> descriptors;
    std::uint64_t next_token = 1; // Zero belongs to the wake descriptor; never reuse tokens.

    explicit Impl(int size) : max_events(size) {
        if (size <= 0) { errno = EINVAL; return; }
        epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0) return;
        wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.u64 = 0;
        if (wake_fd < 0 || ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &event) < 0) {
            const int error = errno;
            if (wake_fd >= 0) ::close(wake_fd);
            ::close(epoll_fd);
            epoll_fd = wake_fd = -1;
            errno = error;
        }
    }
    ~Impl() {
        if (wake_fd >= 0) ::close(wake_fd);
        if (epoll_fd >= 0) ::close(epoll_fd);
    }
};

EpollDemuxer::EpollDemuxer(int max_events) : impl_(std::make_unique<Impl>(max_events)) {}
EpollDemuxer::~EpollDemuxer() { stop(); }
bool EpollDemuxer::is_valid() const { return impl_->epoll_fd >= 0; }
int EpollDemuxer::native_handle() const { return impl_->epoll_fd; }

bool EpollDemuxer::add(int fd, IoEvent events, EventCallback callback, bool edge_trigger) {
    auto& state = *impl_;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!is_valid() || fd < 0) { errno = EBADF; return false; }
    if (state.stopped) { errno = ECANCELED; return false; }
    if (!valid_events(events)) return false;
    if (state.contexts.count(fd)) { errno = EEXIST; return false; }
    if (state.next_token == 0) { errno = EOVERFLOW; return false; }
    const auto token = state.next_token++;
    // Allocate both indexes before ctl. Failed registration leaves neither index
    // nor callback behind, and dispatch cannot see partial state under this lock.
    auto entry = state.contexts.emplace(fd, Impl::Context{{}, token, edge_trigger}).first;
    try {
        state.descriptors.emplace(token, fd);
    } catch (...) {
        state.contexts.erase(entry);
        throw;
    }
    epoll_event event{};
    event.events = native_events(events, edge_trigger);
    event.data.u64 = token;
    if (::epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        state.descriptors.erase(token);
        state.contexts.erase(entry);
        return false;
    }
    entry->second.callback = std::move(callback);
    return true;
}

bool EpollDemuxer::modify(int fd, IoEvent events) {
    auto& state = *impl_;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!is_valid()) { errno = EBADF; return false; }
    if (state.stopped) { errno = ECANCELED; return false; }
    if (!valid_events(events)) return false;
    const auto entry = state.contexts.find(fd);
    if (entry == state.contexts.end()) { errno = ENOENT; return false; }
    epoll_event event{};
    event.events = native_events(events, entry->second.edge_trigger);
    event.data.u64 = entry->second.token;
    return ::epoll_ctl(state.epoll_fd, EPOLL_CTL_MOD, fd, &event) >= 0;
}

bool EpollDemuxer::set_callback(int fd, EventCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto entry = impl_->contexts.find(fd);
    if (entry == impl_->contexts.end()) { errno = ENOENT; return false; }
    // Destroy replaced application captures after releasing the registration lock.
    entry->second.callback.swap(callback);
    return true;
}

bool EpollDemuxer::remove(int fd) {
    EventCallback retired;
    auto& state = *impl_;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!is_valid()) { errno = EBADF; return false; }
    const auto entry = state.contexts.find(fd);
    if (entry == state.contexts.end()) { errno = ENOENT; return false; }
    const bool ok = ::epoll_ctl(state.epoll_fd, EPOLL_CTL_DEL, fd, nullptr) >= 0;
    // A closed descriptor has already been removed by the kernel. Retire its token
    // even when ctl reports EBADF/ENOENT, so buffered events cannot reach a reused fd.
    if (!ok && errno != EBADF && errno != ENOENT) return false;
    retired.swap(entry->second.callback);
    state.descriptors.erase(entry->second.token);
    state.contexts.erase(entry);
    return ok;
}

void EpollDemuxer::run(int timeout_ms) {
    auto& state = *impl_;
    if (!is_valid()) { errno = EBADF; return; }
    if (timeout_ms < -1) { errno = EINVAL; return; }
    if (state.running.exchange(true)) { errno = EBUSY; return; }
    struct RunnerGuard {
        std::atomic<bool>& running;
        ~RunnerGuard() { running = false; }
    } guard{state.running};
    if (state.stopped) return;
    std::vector<epoll_event> events(state.max_events);
    while (!state.stopped) {
        const int count = ::epoll_wait(state.epoll_fd, events.data(), state.max_events, timeout_ms);
        if (count < 0) {
            if (errno == EINTR) continue;
            CHWELL_LOG_ERROR("EpollDemuxer: epoll_wait failed: " << std::strerror(errno));
            return;
        }
        for (int i = 0; i < count && !state.stopped; ++i) {
            const auto token = events[i].data.u64;
            if (token == 0) {
                std::uint64_t value;
                while (true) {
                    if (::read(state.wake_fd, &value, sizeof(value)) > 0) continue;
                    if (errno == EINTR) continue;
                    break;
                }
                continue;
            }
            EventCallback callback;
            int fd;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                const auto descriptor = state.descriptors.find(token);
                if (descriptor == state.descriptors.end()) continue;
                fd = descriptor->second;
                callback = state.contexts.at(fd).callback;
            }
            if (callback) callback(fd, portable_events(events[i].events));
        }
    }
}

bool EpollDemuxer::wake() {
    if (impl_->wake_fd < 0) { errno = EBADF; return false; }
    const std::uint64_t value = 1;
    while (::write(impl_->wake_fd, &value, sizeof(value)) < 0) {
        if (errno == EINTR) continue;
        return errno == EAGAIN; // A saturated counter already guarantees readiness.
    }
    return true;
}

void EpollDemuxer::stop() {
    if (!impl_->stopped.exchange(true)) wake();
}

} // namespace net
} // namespace chwell
