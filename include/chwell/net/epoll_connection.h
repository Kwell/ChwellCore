#pragma once

#include <memory>
#include <vector>
#include <string_view>
#include <functional>
#include <mutex>
#include <atomic>
#include <deque>

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

private:
    void do_read();
    void do_write();
    void update_epoll_events();
    void cleanup();

    int fd_;
    EpollDemuxer* demuxer_;

    std::vector<char> read_buffer_;
    static constexpr size_t kReadBufferSize = 8192;

    std::deque<std::vector<char>> write_queue_;
    std::mutex write_mutex_;
    std::atomic<bool> writing_{false};

    EpollMessageCallback message_cb_;
    EpollConnectionCallback close_cb_;

    std::atomic<bool> closed_{false};
};

} // namespace net
} // namespace chwell
