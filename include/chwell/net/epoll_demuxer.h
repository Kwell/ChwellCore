#pragma once

#include "chwell/net/io_demuxer.h"
#include <memory>

namespace chwell {
namespace net {

// POSIX helpers, implemented with the Linux backend in Chwell::core.
bool set_nonblocking(int fd);
int create_nonblocking_socket(int domain, int type, int protocol = 0);

// Linux readiness backend. This declaration has no native system-header dependency.
// Link Chwell::core to instantiate it; Chwell::io alone only provides the interface.
class EpollDemuxer final : public IoDemuxer {
public:
    explicit EpollDemuxer(int max_events = 1024);
    ~EpollDemuxer() override;
    EpollDemuxer(const EpollDemuxer&) = delete;
    EpollDemuxer& operator=(const EpollDemuxer&) = delete;

    bool is_valid() const override;
    int native_handle() const;
    bool add(int fd, IoEvent events, EventCallback callback, bool edge_trigger = false) override;
    bool modify(int fd, IoEvent events) override;
    bool set_callback(int fd, EventCallback callback) override;
    bool remove(int fd) override;
    void run(int timeout_ms = -1) override;
    bool wake() override;
    void stop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace net
} // namespace chwell
