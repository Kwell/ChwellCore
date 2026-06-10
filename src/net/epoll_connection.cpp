#include "chwell/net/epoll_connection.h"
#include "chwell/core/logger.h"
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <netinet/tcp.h>

namespace chwell {
namespace net {

EpollTcpConnection::EpollTcpConnection(int fd, EpollDemuxer* demuxer)
    : fd_(fd), demuxer_(demuxer), read_buffer_(kReadBufferSize) {
    if (fd_ >= 0) {
        set_nonblocking(fd_);
    }
    last_active_time_ = std::chrono::steady_clock::now();
    CHWELL_LOG_DEBUG("EpollTcpConnection created, fd=" << fd_);
}

EpollTcpConnection::~EpollTcpConnection() {
    cleanup();
}

void EpollTcpConnection::bind_demuxer(EpollDemuxer* demuxer) {
    demuxer_ = demuxer;
}

void EpollTcpConnection::start() {
    if (fd_ < 0 || !demuxer_) {
        CHWELL_LOG_ERROR("EpollTcpConnection::start: invalid fd=" << fd_
                         << " or demuxer=" << (void*)demuxer_);
        return;
    }

    int val = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

    CHWELL_LOG_INFO("EpollTcpConnection::start registering fd=" << fd_
                    << " to demuxer epoll_fd=" << demuxer_->native_handle());

    auto self = shared_from_this();
    bool ok = demuxer_->add(fd_, IoEvent::Read | IoEvent::RdHangup,
        [self](int fd, IoEvent events) {
            if (has_event(events, IoEvent::Error)) {
                self->handle_error_event();
                return;
            }
            if (has_event(events, IoEvent::Hangup) || has_event(events, IoEvent::RdHangup)) {
                self->handle_read_event();
                self->close();
                return;
            }
            if (has_event(events, IoEvent::Read)) {
                self->handle_read_event();
            }
        });

    if (!ok) {
        CHWELL_LOG_ERROR("EpollTcpConnection::start: epoll_ctl ADD failed for fd=" << fd_
                         << ", errno=" << errno);
    }

    CHWELL_LOG_DEBUG("EpollTcpConnection started, fd=" << fd_);
}

void EpollTcpConnection::send(const std::vector<char>& data) {
    send(std::string_view(data.data(), data.size()));
}

void EpollTcpConnection::send(std::string_view data) {
    if (closed_ || fd_ < 0) return;

    {
        std::lock_guard<std::mutex> lock(write_mutex_);

        // 🆕 写队列上限检查（P0-2 半包攻击防护）
        if (write_queue_bytes_ + data.size() > max_write_queue_) {
            CHWELL_LOG_WARN("Write queue overflow on fd=" << fd_
                            << ", queue_bytes=" << write_queue_bytes_
                            << "+" << data.size()
                            << " > max=" << max_write_queue_);
            close();
            return;
        }

        write_queue_.emplace_back(data.begin(), data.end());
        write_queue_bytes_ += data.size();
    }

    update_last_active();

    if (!writing_.exchange(true)) {
        update_epoll_events();
    }
}

void EpollTcpConnection::handle_read_event() { do_read(); }
void EpollTcpConnection::handle_write_event() { do_write(); }

void EpollTcpConnection::handle_error_event() {
    CHWELL_LOG_WARN("EpollTcpConnection error on fd=" << fd_);
    close();
}

void EpollTcpConnection::do_read() {
    CHWELL_LOG_DEBUG("EpollTcpConnection::do_read fd=" << fd_);
    while (fd_ >= 0 && !closed_) {
        ssize_t n = ::read(fd_, read_buffer_.data(), read_buffer_.size());
        CHWELL_LOG_DEBUG("EpollTcpConnection::do_read fd=" << fd_ << " n=" << n);
        if (n > 0) {
            // 🆕 读缓冲区上限检查
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                total_read_bytes_ += static_cast<size_t>(n);
                if (total_read_bytes_ > max_read_buffer_) {
                    CHWELL_LOG_WARN("Read buffer overflow on fd=" << fd_
                                    << ", total_read=" << total_read_bytes_
                                    << " > max=" << max_read_buffer_);
                    close();
                    return;
                }
            }

            update_last_active();

            if (message_cb_) {
                message_cb_(shared_from_this(),
                            std::string_view(read_buffer_.data(), static_cast<size_t>(n)));
            }
        } else if (n == 0) {
            close();
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            CHWELL_LOG_WARN("EpollTcpConnection read error: " + std::string(strerror(errno)));
            close();
            return;
        }
    }
}

void EpollTcpConnection::do_write() {
    std::vector<char> buf;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (write_queue_.empty()) {
            writing_ = false;
            update_epoll_events();
            return;
        }
        buf = std::move(write_queue_.front());
        write_queue_.pop_front();
        write_queue_bytes_ -= buf.size();
    }

    const char* ptr = buf.data();
    size_t remaining = buf.size();

    while (remaining > 0 && fd_ >= 0 && !closed_) {
        ssize_t n = ::write(fd_, ptr, remaining);
        if (n > 0) {
            ptr += n;
            remaining -= static_cast<size_t>(n);
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                total_written_bytes_ += static_cast<size_t>(n);
            }
            update_last_active();
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::lock_guard<std::mutex> lock(write_mutex_);
                size_t leftover = buf.size() - (ptr - buf.data());
                write_queue_.push_front(std::vector<char>(ptr, ptr + remaining));
                write_queue_bytes_ += leftover;
                return;
            }
            if (errno == EINTR) continue;
            CHWELL_LOG_ERROR("EpollTcpConnection write error: " + std::string(strerror(errno)));
            close();
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (write_queue_.empty()) {
            writing_ = false;
            update_epoll_events();
        }
    }
}

void EpollTcpConnection::update_epoll_events() {
    if (!demuxer_ || fd_ < 0) return;
    if (writing_) {
        demuxer_->modify(fd_, IoEvent::Read | IoEvent::Write | IoEvent::RdHangup);
    } else {
        demuxer_->modify(fd_, IoEvent::Read | IoEvent::RdHangup);
    }
}

void EpollTcpConnection::close() {
    if (closed_.exchange(true)) return;
    CHWELL_LOG_INFO("EpollTcpConnection closing, fd=" << fd_);

    if (demuxer_ && fd_ >= 0) demuxer_->remove(fd_);
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
    if (close_cb_) close_cb_(shared_from_this());
}

void EpollTcpConnection::cleanup() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    closed_ = true;
}

void EpollTcpConnection::update_last_active() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    last_active_time_ = std::chrono::steady_clock::now();
}

} // namespace net
} // namespace chwell
