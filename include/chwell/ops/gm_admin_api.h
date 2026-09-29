#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "chwell/http/http_request.h"
#include "chwell/http/http_response.h"
#include "chwell/ops/gm_console.h"

namespace chwell {
namespace ops {

// ========== GM 管理 HTTP API ==========
//
// GmAdminApi：把 GmConsole 暴露为简单 HTTP/JSON 接口，可直接挂到 http::HttpServer。
//
// 路由：
//   GET  /gm/commands           → 已注册指令列表
//   GET  /gm/audit?limit=50     → 审计日志
//   POST /gm/invoke             → body: {"actor":"...","command":"...","args":"..."}
//                                 或 query: ?actor=&command=&args=
//   GET  /gm/health             → {"ok":true}
//
// 鉴权：Header "X-GM-Token" 与构造时 token 相等才放行（空 token 关闭鉴权）。
// 响应均为 application/json，UTF-8。

class GmAdminApi {
public:
    explicit GmAdminApi(GmConsole* console, std::string token = std::string())
        : console_(console), token_(std::move(token)) {}

    // 可直接作为 http::HttpServer::set_handler 的回调
    void handle(const http::HttpRequest& req, http::HttpResponse& resp) {
        if (token_.empty() == false) {
            std::string got = req.header("x-gm-token");
            if (got != token_) {
                set_json(resp, 401, "{\"ok\":false,\"error\":\"unauthorized\"}");
                return;
            }
        }

        std::string path = req.path;
        std::string query;
        auto qpos = path.find('?');
        if (qpos != std::string::npos) {
            query = path.substr(qpos + 1);
            path = path.substr(0, qpos);
        }

        if (req.method == "GET" && path == "/gm/health") {
            set_json(resp, 200, "{\"ok\":true}");
            return;
        }
        if (req.method == "GET" && path == "/gm/commands") {
            set_json(resp, 200, list_commands());
            return;
        }
        if (req.method == "GET" && path == "/gm/audit") {
            std::size_t limit = 50;
            auto lim = query_param(query, "limit");
            if (!lim.empty()) {
                int v = std::atoi(lim.c_str());
                if (v > 0 && v <= 500) limit = static_cast<std::size_t>(v);
            }
            set_json(resp, 200, list_audit(limit));
            return;
        }
        if (req.method == "POST" && path == "/gm/invoke") {
            set_json(resp, 200, invoke(req, query));
            return;
        }
        set_json(resp, 404, "{\"ok\":false,\"error\":\"not found\"}");
    }

private:
    static void set_json(http::HttpResponse& resp, int code, const std::string& body) {
        resp.status_code = code;
        resp.reason = code == 200 ? "OK" : (code == 401 ? "Unauthorized" : (code == 404 ? "Not Found" : "Error"));
        resp.body = body;
        resp.set_header("Content-Type", "application/json; charset=utf-8");
        resp.set_header("Content-Length", std::to_string(body.size()));
    }

    static std::string json_escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    static std::string query_param(const std::string& query, const std::string& key) {
        std::size_t pos = 0;
        while (pos < query.size()) {
            std::size_t amp = query.find('&', pos);
            if (amp == std::string::npos) amp = query.size();
            std::string pair = query.substr(pos, amp - pos);
            std::size_t eq = pair.find('=');
            std::string k = eq == std::string::npos ? pair : pair.substr(0, eq);
            std::string v = eq == std::string::npos ? std::string() : pair.substr(eq + 1);
            if (k == key) return v;
            pos = amp + 1;
        }
        return std::string();
    }

    // 简易 JSON 字段提取："key":"value" 或 "key":123（不依赖完整 JSON 库）
    static std::string json_field(const std::string& body, const std::string& key) {
        std::string pat = "\"" + key + "\"";
        std::size_t p = body.find(pat);
        if (p == std::string::npos) return std::string();
        std::size_t colon = body.find(':', p + pat.size());
        if (colon == std::string::npos) return std::string();
        std::size_t i = colon + 1;
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
        if (i >= body.size()) return std::string();
        if (body[i] == '"') {
            std::size_t e = body.find('"', i + 1);
            if (e == std::string::npos) return std::string();
            return body.substr(i + 1, e - i - 1);
        }
        std::size_t e = i;
        while (e < body.size() && !std::isspace(static_cast<unsigned char>(body[e])) && body[e] != ',' && body[e] != '}') {
            ++e;
        }
        return body.substr(i, e - i);
    }

    std::string list_commands() const {
        auto names = console_->command_names();
        std::ostringstream os;
        os << "{\"ok\":true,\"commands\":[";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i) os << ",";
            os << "\"" << json_escape(names[i]) << "\"";
        }
        os << "]}";
        return os.str();
    }

    std::string list_audit(std::size_t limit) const {
        auto log = console_->audit_log(limit);
        std::ostringstream os;
        os << "{\"ok\":true,\"audit\":[";
        for (std::size_t i = 0; i < log.size(); ++i) {
            const auto& r = log[i];
            if (i) os << ",";
            os << "{\"ts\":" << r.time_ms
               << ",\"actor\":\"" << json_escape(r.actor) << "\""
               << ",\"command\":\"" << json_escape(r.command) << "\""
               << ",\"args\":\"" << json_escape(r.args) << "\""
               << ",\"ok\":" << (r.ok ? "true" : "false")
               << ",\"result\":\"" << json_escape(r.result) << "\"}";
        }
        os << "]}";
        return os.str();
    }

    std::string invoke(const http::HttpRequest& req, const std::string& query) {
        std::string actor = json_field(req.body, "actor");
        std::string command = json_field(req.body, "command");
        std::string args = json_field(req.body, "args");
        if (actor.empty()) actor = query_param(query, "actor");
        if (command.empty()) command = query_param(query, "command");
        if (args.empty()) args = query_param(query, "args");

        if (actor.empty() || command.empty()) {
            return "{\"ok\":false,\"error\":\"actor and command required\"}";
        }
        std::string result;
        bool ok = console_->invoke(actor, command, args, result);
        std::ostringstream os;
        os << "{\"ok\":" << (ok ? "true" : "false")
           << ",\"result\":\"" << json_escape(result) << "\"}";
        return os.str();
    }

    GmConsole* console_;
    std::string token_;
};

}  // namespace ops
}  // namespace chwell
