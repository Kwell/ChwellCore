#pragma once

#include <cstdint>
#include <functional>

namespace chwell {
namespace net {

// Framework event bits. Backends translate these to/from native readiness flags.
enum class IoEvent : std::uint32_t {
    None        = 0,
    Read        = 1u << 0,
    Write       = 1u << 1,
    Error       = 1u << 2,
    Hangup      = 1u << 3,
    RdHangup    = 1u << 4,
    Priority    = 1u << 5,
    EdgeTrigger = 1u << 6,
};

constexpr IoEvent operator|(IoEvent a, IoEvent b) noexcept {
    return static_cast<IoEvent>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr IoEvent operator&(IoEvent a, IoEvent b) noexcept {
    return static_cast<IoEvent>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
constexpr bool has_event(IoEvent events, IoEvent flag) noexcept {
    return static_cast<std::uint32_t>(events & flag) != 0;
}

// Descriptors remain POSIX ints during the handle-model migration.
using EventCallback = std::function<void(int fd, IoEvent events)>;

// Readiness interface; the backend owns its poller, the caller owns registered fds.
// Registration operations and wake/stop may be called from other threads.
// Only one concurrent run is supported. stop is permanent, including before run.
// Callbacks run outside registration locks and may modify/remove registrations.
// remove does not cancel a callback already selected for execution. Stop and join
// all runners before destroying the demuxer or callback-owned application state.
// Callback exceptions propagate to the run caller.
class IoDemuxer {
public:
    virtual ~IoDemuxer() = default;
    virtual bool is_valid() const = 0;
    virtual bool add(int fd, IoEvent events, EventCallback callback, bool edge_trigger = false) = 0;
    virtual bool modify(int fd, IoEvent events) = 0;
    virtual bool set_callback(int fd, EventCallback callback) = 0;
    virtual bool remove(int fd) = 0;
    virtual void run(int timeout_ms = -1) = 0;
    // Wake interrupts the backend wait; it neither stops the loop nor emits an IO callback.
    virtual bool wake() = 0;
    virtual void stop() = 0;
};

} // namespace net
} // namespace chwell
