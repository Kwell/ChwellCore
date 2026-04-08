#pragma once

#include "chwell/net/tcp_connection.h"
#include "chwell/net/epoll_connection.h"

namespace chwell {
namespace net {

/**
 * @brief 将 EpollTcpConnection 适配为 TcpConnectionPtr 的桥接类
 *
 * 继承 TcpConnection（虚函数），将 I/O 委托给 EpollTcpConnection。
 * 上层组件无需修改，仍然使用 TcpConnectionPtr。
 */
class EpollTcpBridge : public TcpConnection {
public:
    explicit EpollTcpBridge(const EpollTcpConnectionPtr& conn)
        : TcpConnection(TcpSocket(-1))
        , epoll_conn_(conn)
        , saved_fd_(conn->native_handle()) {}

    void start() override {
        // no-op: epoll manages read events
    }

    void send(const std::vector<char>& data) override {
        if (epoll_conn_) epoll_conn_->send(data);
    }

    void send(std::string_view data) override {
        if (epoll_conn_) epoll_conn_->send(data);
    }

    void close() override {
        if (epoll_conn_) epoll_conn_->close();
    }

    int native_handle() const noexcept override {
        // 返回保存的 fd，因为 epoll_conn 可能已关闭
        return saved_fd_;
    }

    int saved_fd() const noexcept { return saved_fd_; }
    const EpollTcpConnectionPtr& epoll_connection() const { return epoll_conn_; }

private:
    EpollTcpConnectionPtr epoll_conn_;
    int saved_fd_;  // 构造时保存，close 后仍然可用
};

typedef std::shared_ptr<EpollTcpBridge> EpollTcpBridgePtr;

} // namespace net
} // namespace chwell
