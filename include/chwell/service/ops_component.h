#pragma once

// 运维管理组件：通过游戏协议暴露运行时运维命令
//
// 命令字（0x03xx 运维段）：
//   0x0301 C2S_OPS_QUERY     → S2C_OPS_STATUS   （JSON 状态快照）
//   0x0302 C2S_OPS_METRICS   → S2C_OPS_METRICS  （Prometheus 文本）
//   0x0303 C2S_OPS_SET_LOG   → S2C_OPS_ACK      （body: "DEBUG|INFO|WARN|ERROR"）
//   0x0304 C2S_OPS_RESET_METRICS → S2C_OPS_ACK
//
// 安全：生产环境必须 set_auth_token 并在请求 body 前缀携带 token；
// 未设置 token 时仅在非生产（allow_unauthenticated=true）下放行。

#include <string>
#include <string_view>
#include <cstdint>

#include "chwell/service/component.h"
#include "chwell/service/protocol_router.h"
#include "chwell/protocol/message.h"
#include "chwell/metrics/instrumentation.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace service {

namespace ops_cmd {
const std::uint16_t C2S_OPS_QUERY        = 0x0301;
const std::uint16_t S2C_OPS_STATUS       = 0x0302;
const std::uint16_t C2S_OPS_METRICS      = 0x0303;
const std::uint16_t S2C_OPS_METRICS      = 0x0304;
const std::uint16_t C2S_OPS_SET_LOG      = 0x0305;
const std::uint16_t S2C_OPS_ACK          = 0x0306;
const std::uint16_t C2S_OPS_RESET_METRICS = 0x0307;
const std::uint16_t S2C_OPS_ERROR        = 0x03FF;
} // namespace ops_cmd

class OpsComponent : public Component {
public:
    virtual std::string name() const override { return "OpsComponent"; }

    // 运维口令：body 格式为 [token_len:2B BE][token][payload]
    void set_auth_token(std::string token) {
        auth_token_ = std::move(token);
    }

    // 未设置 token 时是否放行（开发环境用；生产必须 false）
    void set_allow_unauthenticated(bool allow) {
        allow_unauth_ = allow;
    }

    virtual void on_register(Service& svc) override {
        auto* router = svc.get_component<ProtocolRouterComponent>();
        if (!router) {
            CHWELL_LOG_ERROR("OpsComponent requires ProtocolRouterComponent");
            return;
        }
        router->register_handler(ops_cmd::C2S_OPS_QUERY,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                handle_query(conn, msg);
            });
        router->register_handler(ops_cmd::C2S_OPS_METRICS,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                handle_metrics(conn, msg);
            });
        router->register_handler(ops_cmd::C2S_OPS_SET_LOG,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                handle_set_log(conn, msg);
            });
        router->register_handler(ops_cmd::C2S_OPS_RESET_METRICS,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                handle_reset(conn, msg);
            });
        CHWELL_LOG_INFO("OpsComponent registered (auth=" +
                        std::string(auth_token_.empty() ? "none" : "token") + ")");
    }

private:
    // 鉴权：token 形态 [len:2B BE][token][payload]；无 token 配置且允许匿名时放行
    bool authorized(const protocol::Message& msg, std::string& out_payload) {
        out_payload.assign(msg.body.begin(), msg.body.end());
        if (auth_token_.empty()) {
            return allow_unauth_;
        }
        if (msg.body.size() < 2) return false;
        std::uint16_t tlen = static_cast<std::uint8_t>(msg.body[0]) << 8 |
                             static_cast<std::uint8_t>(msg.body[1]);
        if (msg.body.size() < 2u + tlen) return false;
        std::string token(msg.body.begin() + 2, msg.body.begin() + 2 + tlen);
        if (token != auth_token_) return false;
        out_payload.assign(msg.body.begin() + 2 + tlen, msg.body.end());
        return true;
    }

    void reply_err(const net::TcpConnectionPtr& conn, const std::string& why) {
        ProtocolRouterComponent::send_message(
            conn, protocol::Message(ops_cmd::S2C_OPS_ERROR, why));
    }

    void handle_query(const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
        std::string payload;
        if (!authorized(msg, payload)) {
            reply_err(conn, "unauthorized");
            return;
        }
        auto json = metrics::Instrumentation::instance().render_status_json();
        ProtocolRouterComponent::send_message(
            conn, protocol::Message(ops_cmd::S2C_OPS_STATUS, json));
    }

    void handle_metrics(const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
        std::string payload;
        if (!authorized(msg, payload)) {
            reply_err(conn, "unauthorized");
            return;
        }
        auto text = metrics::Instrumentation::instance().render_prometheus();
        ProtocolRouterComponent::send_message(
            conn, protocol::Message(ops_cmd::S2C_OPS_METRICS, text));
    }

    void handle_set_log(const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
        std::string payload;
        if (!authorized(msg, payload)) {
            reply_err(conn, "unauthorized");
            return;
        }
        // payload: 级别字符串
        std::string lvl = payload;
        core::LogLevel level;
        if (lvl == "DEBUG" || lvl == "debug") level = core::LogLevel::Debug;
        else if (lvl == "INFO" || lvl == "info") level = core::LogLevel::Info;
        else if (lvl == "WARN" || lvl == "warn") level = core::LogLevel::Warn;
        else if (lvl == "ERROR" || lvl == "error") level = core::LogLevel::Error;
        else {
            reply_err(conn, "unknown level: " + lvl);
            return;
        }
        core::Logger::instance().set_level(level);
        CHWELL_LOG_INFO("Ops: log level set to " + lvl);
        ProtocolRouterComponent::send_message(
            conn, protocol::Message(ops_cmd::S2C_OPS_ACK, "ok"));
    }

    void handle_reset(const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
        std::string payload;
        if (!authorized(msg, payload)) {
            reply_err(conn, "unauthorized");
            return;
        }
        metrics::Instrumentation::instance().reset();
        ProtocolRouterComponent::send_message(
            conn, protocol::Message(ops_cmd::S2C_OPS_ACK, "reset"));
    }

    std::string auth_token_;
    bool allow_unauth_ = true;
};

} // namespace service
} // namespace chwell
