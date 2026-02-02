#pragma once

#include <functional>
#include <vector>
#include <thread>
#include <atomic>

#include "chwell/net/posix_io.h"

namespace chwell {
namespace net {

typedef std::function<void(const std::vector<char>& data,
                           const UdpEndpoint& remote)> UdpMessageCallback;

// 简单的 UDP 服务器封装：单端口收发，使用线程 + 阻塞 recvfrom
class UdpServer {
public:
    UdpServer(IoService& io_service, unsigned short port);

    void start_receive();
    void stop();

    void send_to(const std::vector<char>& data, const UdpEndpoint& remote);

    void set_message_callback(const UdpMessageCallback& cb) { message_cb_ = cb; }

private:
    void recv_loop();

    IoService& io_service_;
    int fd_{-1};
    std::vector<char> buffer_;
    UdpMessageCallback message_cb_;
    std::thread recv_thread_;
    std::atomic<bool> stopped_{false};
};

} // namespace net
} // namespace chwell
