#pragma once

#include <asio.hpp>
#include <functional>
#include <memory>

#include "chwell/http/http_request.h"
#include "chwell/http/http_response.h"

namespace chwell {
namespace http {

typedef std::function<void(const HttpRequest&, HttpResponse&)> HttpHandler;

// 一个简单的 HTTP 服务器：支持单次请求-响应（短连接），适合作为管理/监控接口或简单 HTTP API。
class HttpServer {
public:
    HttpServer(asio::io_service& io_service, unsigned short port);

    void start();

    void set_handler(const HttpHandler& handler) { handler_ = handler; }

private:
    void do_accept();

    asio::io_service& io_service_;
    asio::ip::tcp::acceptor acceptor_;
    HttpHandler handler_;
};

} // namespace http
} // namespace chwell

