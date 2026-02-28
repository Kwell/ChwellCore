#pragma once

#include <functional>
#include <memory>
#include <thread>
#include <atomic>

#include "chwell/net/posix_io.h"
#include "chwell/http/http_request.h"
#include "chwell/http/http_response.h"

namespace chwell {
namespace http {

typedef std::function<void(const HttpRequest&, HttpResponse&)> HttpHandler;

// 一个简单的 HTTP 服务器：支持单次请求-响应（短连接）
class HttpServer {
public:
    HttpServer(net::IoService& io_service, unsigned short port);

    void start();
    void stop();

    void set_handler(const HttpHandler& handler) { handler_ = handler; }

private:
    void accept_loop();

    net::IoService& io_service_;
    unsigned short port_;
    net::TcpAcceptor acceptor_;
    int wake_pipe_[2]{-1, -1};
    std::thread accept_thread_;
    std::atomic<bool> stopped_{false};
    HttpHandler handler_;
};

} // namespace http
} // namespace chwell
