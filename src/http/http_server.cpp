#include "chwell/http/http_server.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace http {

namespace {

// 连接会话，负责读取并解析一个 HTTP 请求，然后调用用户 handler，最后发送响应并关闭。
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(asio::ip::tcp::socket socket, const HttpHandler& handler)
        : socket_(std::move(socket)),
          handler_(handler),
          buffer_(8192) {
    }

    void start() {
        do_read();
    }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(asio::buffer(buffer_),
            [this, self](const asio::error_code& ec, std::size_t bytes_transferred) {
                if (ec) {
                    if (ec != asio::error::operation_aborted) {
                        core::Logger::instance().warn("HttpSession read error: " + ec.message());
                    }
                    return;
                }

                request_data_.append(buffer_.data(), bytes_transferred);

                // 简单解析：查找头部结束标记 \r\n\r\n
                std::size_t pos = request_data_.find("\r\n\r\n");
                if (pos == std::string::npos) {
                    // 头部尚未完整，继续读
                    do_read();
                    return;
                }

                // 解析请求
                HttpRequest req;
                if (!parse_request(request_data_, req)) {
                    core::Logger::instance().warn("HttpSession parse request failed");
                    close();
                    return;
                }

                HttpResponse resp;
                if (handler_) {
                    handler_(req, resp);
                } else {
                    resp.status_code = 404;
                    resp.reason = "Not Found";
                    resp.body = "No handler";
                    resp.set_header("Content-Type", "text/plain; charset=utf-8");
                }

                auto bytes = resp.to_bytes();
                do_write(bytes);
            });
    }

    // 非严格的 HTTP 请求解析器，仅支持简单的 GET/POST，单次请求。
    bool parse_request(const std::string& data, HttpRequest& req) {
        std::size_t pos = data.find("\r\n");
        if (pos == std::string::npos) {
            return false;
        }

        std::string request_line = data.substr(0, pos);

        // 解析请求行：METHOD SP PATH SP VERSION
        std::size_t sp1 = request_line.find(' ');
        if (sp1 == std::string::npos) return false;
        std::size_t sp2 = request_line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return false;

        req.method = request_line.substr(0, sp1);
        req.path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        req.version = request_line.substr(sp2 + 1);

        // 解析头部
        std::size_t header_start = pos + 2;
        std::size_t header_end = data.find("\r\n\r\n");
        if (header_end == std::string::npos) return false;

        std::size_t line_start = header_start;
        while (line_start < header_end) {
            std::size_t line_end = data.find("\r\n", line_start);
            if (line_end == std::string::npos || line_end > header_end) break;

            std::string line = data.substr(line_start, line_end - line_start);
            std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                // 去掉前导空格
                while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
                    value.erase(value.begin());
                }
                req.headers[key] = value;
            }

            line_start = line_end + 2;
        }

        // 解析 body（如果有 Content-Length）
        std::string content_length_str = req.header("Content-Length");
        if (!content_length_str.empty()) {
            int len = std::atoi(content_length_str.c_str());
            std::size_t body_start = header_end + 4;
            if (len > 0 && body_start + static_cast<std::size_t>(len) <= data.size()) {
                req.body = data.substr(body_start, static_cast<std::size_t>(len));
            }
        }

        return true;
    }

    void do_write(const std::vector<char>& data) {
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(data),
            [this, self](const asio::error_code& ec, std::size_t /*bytes_transferred*/) {
                if (ec) {
                    core::Logger::instance().warn("HttpSession write error: " + ec.message());
                }
                close();
            });
    }

    void close() {
        asio::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

    asio::ip::tcp::socket socket_;
    HttpHandler handler_;
    std::string request_data_;
    std::vector<char> buffer_;
};

} // anonymous namespace

HttpServer::HttpServer(asio::io_service& io_service, unsigned short port)
    : io_service_(io_service),
      acceptor_(io_service, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {
}

void HttpServer::start() {
    do_accept();
}

void HttpServer::do_accept() {
    acceptor_.async_accept(
        [this](const asio::error_code& ec, asio::ip::tcp::socket socket) {
            if (ec) {
                core::Logger::instance().error("HttpServer accept failed: " + ec.message());
                do_accept();
                return;
            }

            std::shared_ptr<HttpSession> session =
                std::make_shared<HttpSession>(std::move(socket), handler_);
            session->start();

            do_accept();
        });
}

} // namespace http
} // namespace chwell

