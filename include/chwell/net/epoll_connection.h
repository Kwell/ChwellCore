#pragma once

#include <memory>
#include <vector>
#include <string_view>
#include <functional>
#include <mutex>
#include <atomic>
#include <deque>
#include <chrono>

#include "chwell/net/posix_io.h"
#include "chwell/net/epoll_demuxer.h"

namespace chwell {
namespace net {

class EpollTcpConnection;
class EpollTcpServer;

typedef std::shared_ptr<EpollTcpConnection> EpollTcpConnectionPtr;
typedef std::function<void(const EpollTcpConnectionPtr&, std::string_view)> EpollMessageCallback;
typedef std::function<void(const EpollTcpConnectionPtr&)> EpollConnectionCallback;

/**
 * @brief 事件驱动的 TCP 连接（epoll + non-blocking）
 *
 * 对外接口与原 TcpConnection 一致，内部改为：
 * - 非阻塞 socket
 * - 读事件由 epoll 驱动（EPOLLIN + ET）
 * - 写操作先入缓冲队列，EPOLLOUT 时刷出
 *
 * 安全特性（P0-2 半包攻击防护）：
 * - max_read_buffer_: 读缓冲区上限，超过则关闭连接
 * - max_write_queue_: 写队列上限，超过则关闭连接
 * - idle_timeout_sec_: 空闲超时，由 Server 周期检查
 * - last_active_time_: 最后活跃时间（读或写）
 */
class EpollTcpConnection : public std::enable_shared_from_this<EpollTcpConnection> {
public:
    explicit EpollTcpConnection(int fd, EpollDemuxer* demuxer = nullptr);
    ~EpollTcpConnection();

    void bind_demuxer(EpollDemuxer* demuxer);
    void start();

    void send(const std::vector<char>& data);
    void send(std::string_view data);
    void close();

    void set_message_callback(const EpollMessageCallback& cb) { message_cb_ = cb; }
    void set_close_callback(const EpollConnectionCallback& cb) { close_cb_ = cb; }

    int native_handle() const noexcept { return fd_; }
    bool is_open() const noexcept { return fd_ >= 0 && !closed_; }

    void handle_read_event();
    void handle_write_event();
    void handle_error_event();

    // ========== 安全限制（P0-2） ==========

    // 设置读缓冲区上限（字节），默认 64KB
    void set_max_read_buffer(size_t max_bytes) { max_read_buffer_ = max_bytes; }
    size_t max_read_buffer() const { return max_read_buffer_; }

    // 设置写队列上限（字节），默认 1MB
    void set_max_write_queue(size_t max_bytes) { max_write_queue_ = max_bytes; }
    size_t max_write_queue() const { return max_write_queue_; }

    // 设置空闲超时（秒），0 表示不超时，默认 30 秒
    void set_idle_timeout(int seconds) { idle_timeout_sec_ = seconds; }
    int idle_timeout_sec() const { return idle_timeout_sec_; }

    // 最后活跃时间
    std::chrono::steady_clock::time_point last_active_time() const {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        return last_active_time_;
    }

    // 当前写队列字节数
    size_t write_queue_bytes() const {
        std::lock_guard<std::mutex> lock(write_mutex_);
        return write_queue_bytes_;
    }

    // 累积读入字节数
    size_t total_read_bytes() const {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        return total_read_bytes_;
    }

    // 累积写出字节数
    size_t total_written_bytes() const {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        return total_written_bytes_;
    }

    // 检查是否空闲超时（由 Server 周期调用）
    bool is_idle_timeout() const {
        if (idle_timeout_sec_ <= 0) return false;
        auto now = std::chrono::steady_clock::now();
        auto last = last_active_time();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last).count();
        return elapsed > idle_timeout_sec_;
    }

private:
    void do_read();
    void do_write();
    void update_epoll_events();
    void cleanup();
    void update_last_active();

    int fd_;
    EpollDemuxer* demuxer_;

    std::vector<char> read_buffer_;
    static constexpr size_t kReadBufferSize = 8192;

    mutable std::mutex write_mutex_;
    std::deque<std::vector<char>> write_queue_;
    size_t write_queue_bytes_ = 0;  // 写队列累积字节数
    std::atomic<bool> writing_{false};

    EpollMessageCallback message_cb_;
    EpollConnectionCallback close_cb_;

    std::atomic<bool> closed_{false};

    // ========== 安全限制相关 ==========
    size_t max_read_buffer_ = 64 * 1024;        // 64KB
    size_t max_write_queue_ = 1024 * 1024;      // 1MB
    int idle_timeout_sec_ = 30;                  // 30 秒

    mutable std::mutex stats_mutex_;
    std::chrono::steady_clock::time_point last_active_time_;
    size_t total_read_bytes_ = 0;
    size_t total_written_bytes_ = 0;
};

} // namespace net
} // namespace chwell
