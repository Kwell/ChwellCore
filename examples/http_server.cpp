#include <csignal>
#include <iostream>
#include <unistd.h>

#include "chwell/core/logger.h"
#include "chwell/core/config.h"
#include "chwell/core/thread_pool.h"
#include "chwell/http/http_server.h"

using namespace chwell;

int main() {
    core::Logger::instance().info("Starting HTTP Server...");

    core::Config cfg;
    cfg.load_from_file("http.conf");

    net::IoService io_service;

    http::HttpServer server(io_service, 8080);

    server.set_handler([](const http::HttpRequest& req, http::HttpResponse& resp) {
        core::Logger::instance().info("HTTP " + req.method + " " + req.path);

        if (req.path == "/") {
            resp.status_code = 200;
            resp.reason = "OK";
            resp.set_header("Content-Type", "text/plain; charset=utf-8");
            resp.body = "ChwellFrameCore HTTP Server\n";
        } else if (req.path == "/health") {
            resp.status_code = 200;
            resp.reason = "OK";
            resp.set_header("Content-Type", "application/json; charset=utf-8");
            resp.body = "{\"status\":\"ok\"}";
        } else {
            resp.status_code = 404;
            resp.reason = "Not Found";
            resp.set_header("Content-Type", "text/plain; charset=utf-8");
            resp.body = "404 Not Found";
        }
    });

    server.start();

    core::ThreadPool pool(4);
    for (int i = 0; i < 4; ++i) {
        pool.post([&io_service]() {
            io_service.run();
        });
    }

    core::Logger::instance().info("HTTP Server listening on port 8080");
    // 交互模式等待回车，Docker/后台模式等待 SIGTERM/SIGINT
    static volatile sig_atomic_t g_stop = 0;
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });

    if (isatty(STDIN_FILENO)) {
        std::cout << "Press ENTER to exit..." << std::endl;
        std::string line;
        std::getline(std::cin, line);
    } else {
        while (!g_stop) {
            sleep(1);
        }
    }

    server.stop();
    io_service.stop();
    return 0;
}
