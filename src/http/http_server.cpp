#include "chwell/http/http_server.h"
#include "chwell/core/logger.h"
#include <cstring>
#include <poll.h>
#include <unistd.h>
#include <cerrno>

namespace chwell {
namespace http {

namespace {

// 连接会话，负责读取并解析一个 HTTP 请求，然后调用用户 handler，最后发送响应并关闭。
class HttpSession {
public:
    HttpSession(net::TcpSocket socket, const HttpHandler& handler)
        : socket_(std::move(socket)),
          handler_(handler),
          buffer_(8192) {
    }

    void run() {
        std::string request_data;
        request_data.reserve(8192);

        while (socket_.is_open()) {
            ssize_t n = socket_.read(buffer_.data(), buffer_.size());
            if (n <= 0) break;

            request_data.append(buffer_.data(), static_cast<std::size_t>(n));

            std::size_t pos = request_data.find("\r\n\r\n");
            if (pos == std::string::npos) continue;

            HttpRequest req;
            if (!parse_request(request_data, req)) {
                core::Logger::instance().warn("HttpSession parse request failed");
                break;
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
            const char* ptr = bytes.data();
            std::size_t len = bytes.size();
            while (len > 0) {
                ssize_t written = socket_.write(ptr, len);
                if (written <= 0) break;
                ptr += written;
                len -= static_cast<std::size_t>(written);
            }
            break;  // 短连接，处理完一个请求后关闭
        }

        net::ErrorCode ec;
        socket_.shutdown(SHUT_RDWR, ec);
        socket_.close(ec);
    }

private:
    bool parse_request(const std::string& data, HttpRequest& req) {
        std::size_t pos = data.find("\r\n");
        if (pos == std::string::npos) return false;

        std::string request_line = data.substr(0, pos);

        std::size_t sp1 = request_line.find(' ');
        if (sp1 == std::string::npos) return false;
        std::size_t sp2 = request_line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return false;

        req.method = request_line.substr(0, sp1);
        req.path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        req.version = request_line.substr(sp2 + 1);

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
                while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
                    value.erase(value.begin());
                }
                req.headers[key] = value;
            }

            line_start = line_end + 2;
        }

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

    net::TcpSocket socket_;
    HttpHandler handler_;
    std::vector<char> buffer_;
};

} // anonymous namespace

HttpServer::HttpServer(chwell::net::IoService& io_service, unsigned short port)
    : io_service_(io_service), acceptor_(port) {
}

void HttpServer::start() {
    if (acceptor_.listen_fd() < 0) {
        core::Logger::instance().error("HttpServer: failed to create acceptor");
        return;
    }

    if (pipe(wake_pipe_) != 0) {
        core::Logger::instance().error("HttpServer: failed to create wake pipe");
        return;
    }

    stopped_ = false;
    accept_thread_ = std::thread([this]() { accept_loop(); });
}

void HttpServer::stop() {
    stopped_ = true;
    if (wake_pipe_[1] >= 0) {
        char c = 1;
        ssize_t n = write(wake_pipe_[1], &c, 1);
        (void)n;  // best-effort wake during shutdown
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (wake_pipe_[0] >= 0) { close(wake_pipe_[0]); wake_pipe_[0] = -1; }
    if (wake_pipe_[1] >= 0) { close(wake_pipe_[1]); wake_pipe_[1] = -1; }
}

void HttpServer::accept_loop() {
    pollfd fds[2];
    fds[0].fd = acceptor_.listen_fd();
    fds[0].events = POLLIN;
    fds[1].fd = wake_pipe_[0];
    fds[1].events = POLLIN;

    while (!stopped_) {
        int ret = poll(fds, 2, 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            core::Logger::instance().error("HttpServer poll error: " + std::string(strerror(errno)));
            break;
        }
        if (ret == 0) continue;

        if (fds[1].revents & POLLIN) {
            char buf[64];
            ssize_t n = read(wake_pipe_[0], buf, sizeof(buf));
            (void)n;  // drain wake pipe
            break;
        }

        if (fds[0].revents & POLLIN) {
            net::ErrorCode ec;
            net::TcpSocket socket = acceptor_.accept(ec);
            if (ec) {
                core::Logger::instance().error("HttpServer accept failed: " + ec.message());
                continue;
            }

            std::shared_ptr<HttpSession> session =
                std::make_shared<HttpSession>(std::move(socket), handler_);
            io_service_.post([session]() {
                session->run();
            });
        }
    }
}

} // namespace http
} // namespace chwell
