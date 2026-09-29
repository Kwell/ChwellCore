// GM 管理服务示例：
// 把 GmConsole / GmAdminApi / Analytics / 支付 mock 挂到 HTTP 上。
//
// 用法：
//   ./example_gm_admin [port] [token]
//
// 接口：
//   GET  /gm/health
//   GET  /gm/commands
//   GET  /gm/audit?limit=50
//   POST /gm/invoke     body: {"actor":"root","command":"echo","args":"hi"}
//   GET  /analytics/top?n=5
//   POST /pay/create    body: {"user":"u1","product":"sku","amount":100}

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "chwell/core/logger.h"
#include "chwell/core/thread_pool.h"
#include "chwell/http/http_server.h"
#include "chwell/ops/analytics.h"
#include "chwell/ops/gm_admin_api.h"
#include "chwell/ops/gm_console.h"
#include "chwell/pay/payment_gateway.h"

using namespace chwell;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

}  // namespace

int main(int argc, char** argv) {
    unsigned short port = 8090;
    std::string token = "gm-dev-token";
    if (argc > 1) port = static_cast<unsigned short>(std::atoi(argv[1]));
    if (argc > 2) token = argv[2];

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    CHWELL_LOG_INFO("Starting GM admin on port " << port);

    ops::GmConsole console;
    console.set_actor_level("root", 0);
    console.set_actor_level("admin", 0);

    // 内置示例指令
    console.register_command("echo", 0,
        [](const std::string&, const std::string& args, std::string& result) {
            result = args;
            return true;
        });
    console.register_command("set_log_level", 1,
        [](const std::string&, const std::string& args, std::string& result) {
            result = "log level -> " + args;
            return true;
        });
    console.register_command("kick_all", 10,
        [](const std::string&, const std::string&, std::string& result) {
            result = "kicked 0 players (demo)";
            return true;
        });

    ops::AnalyticsPipeline analytics;
    analytics.track("demo_start", "system", 1);

    pay::MemoryPaymentGateway payments;

    ops::GmAdminApi gm_api(&console, token);

    net::IoService io_service;
    http::HttpServer server(io_service, port);

    server.set_handler([&](const http::HttpRequest& req, http::HttpResponse& resp) {
        std::string path = req.path;
        auto qpos = path.find('?');
        std::string query = qpos == std::string::npos ? std::string() : path.substr(qpos + 1);
        if (qpos != std::string::npos) path = path.substr(0, qpos);

        if (path.rfind("/gm", 0) == 0) {
            gm_api.handle(req, resp);
            return;
        }
        if (req.method == "GET" && path == "/analytics/top") {
            auto top = analytics.top_events(5);
            std::string body = "{\"ok\":true,\"events\":[";
            for (std::size_t i = 0; i < top.size(); ++i) {
                if (i) body += ",";
                body += "{\"name\":\"" + top[i].first + "\",\"count\":" + std::to_string(top[i].second) + "}";
            }
            body += "]}";
            resp.status_code = 200;
            resp.reason = "OK";
            resp.body = body;
            resp.set_header("Content-Type", "application/json; charset=utf-8");
            resp.set_header("Content-Length", std::to_string(body.size()));
            return;
        }
        if (req.method == "POST" && path == "/pay/create") {
            // 极简解析：{"user":"..","product":"..","amount":123}
            auto grab = [&](const char* key) {
                std::string pat = std::string("\"") + key + "\"";
                auto p = req.body.find(pat);
                if (p == std::string::npos) return std::string();
                auto c = req.body.find(':', p);
                if (c == std::string::npos) return std::string();
                auto q = req.body.find('"', c);
                if (q == std::string::npos) return std::string();
                auto e = req.body.find('"', q + 1);
                return e == std::string::npos ? std::string() : req.body.substr(q + 1, e - q - 1);
            };
            std::string user = grab("user");
            std::string product = grab("product");
            std::string amount_s = grab("amount");
            std::int64_t amount = amount_s.empty() ? 0 : std::atoll(amount_s.c_str());
            auto order = payments.create_order(user, product, amount, "CNY", 0);
            std::string body = order.order_id.empty()
                ? std::string("{\"ok\":false,\"error\":\"create failed\"}")
                : "{\"ok\":true,\"order_id\":\"" + order.order_id + "\"}";
            resp.status_code = 200;
            resp.reason = "OK";
            resp.body = body;
            resp.set_header("Content-Type", "application/json; charset=utf-8");
            return;
        }

        resp.status_code = 404;
        resp.reason = "Not Found";
        resp.set_header("Content-Type", "text/plain; charset=utf-8");
        resp.body = "Not Found\n";
    });

    server.start();
    CHWELL_LOG_INFO("GM admin listening on :" << port << " token=" << token);

    core::ThreadPool pool(2);
    for (int i = 0; i < 2; ++i) {
        pool.post([&io_service]() { io_service.run(); });
    }

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    CHWELL_LOG_INFO("Shutting down GM admin");
    server.stop();
    io_service.stop();
    return 0;
}
