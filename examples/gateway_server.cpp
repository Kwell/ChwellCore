#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

#include "chwell/core/logger.h"
#include "chwell/core/config.h"
#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/service/session_component.h"
#include "chwell/gateway/gateway_forwarder.h"
#include "chwell/protocol/message.h"

using namespace chwell;

// 命令号（与 protocol_server 保持一致）
namespace Cmd {
    const std::uint16_t ECHO      = 1;
    const std::uint16_t CHAT      = 2;
    const std::uint16_t HEARTBEAT = 3;
    const std::uint16_t LOGIN     = 10;
    const std::uint16_t LOGOUT    = 11;
}

int main() {
    CHWELL_LOG_INFO("Starting Gateway Server...");

    core::Config cfg;
    cfg.load_from_file("gateway.conf");

    // 网关监听端口（缺省 9001，可通过 GATEWAY_PORT 或 listen_port 覆盖）
    int port = 9001;
    if (const char* env = std::getenv("GATEWAY_PORT")) {
        int p = std::atoi(env);
        if (p > 0) port = p;
    } else if (cfg.listen_port() > 0) {
        port = cfg.listen_port();
    }
    unsigned short gateway_port = static_cast<unsigned short>(port);

    // 后端逻辑服地址（缺省 127.0.0.1:9000，可通过 BACKEND_HOST/BACKEND_PORT 覆盖）
    std::string backend_host = "127.0.0.1";
    if (const char* env = std::getenv("BACKEND_HOST")) {
        backend_host = env;
    }
    unsigned short backend_port = 9000;
    if (const char* env = std::getenv("BACKEND_PORT")) {
        backend_port = static_cast<unsigned short>(std::atoi(env));
    }

    // 创建网关服务
    service::Service svc(gateway_port, static_cast<std::size_t>(cfg.worker_threads()));

    // 注册协议路由组件
    auto* router = svc.add_component<service::ProtocolRouterComponent>();

    // 注册会话组件（网关本地处理登录）
    auto* session = svc.add_component<service::SessionComponent>();

    // 注册转发组件（转发到后端逻辑服）
    auto* forwarder = svc.add_component<gateway::GatewayForwarderComponent>(
        backend_host, backend_port);

    // LOGIN：网关本地处理
    router->register_handler(Cmd::LOGIN,
        [session](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
            std::string player_id(msg.body.begin(), msg.body.end());
            if (player_id.empty()) {
                protocol::Message reply(Cmd::LOGIN, "login failed: empty player_id");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            session->login(conn, player_id);
            protocol::Message reply(Cmd::LOGIN, "login ok: " + player_id);
            service::ProtocolRouterComponent::send_message(conn, reply);
        });

    // LOGOUT：网关本地处理
    router->register_handler(Cmd::LOGOUT,
        [session](const net::TcpConnectionPtr& conn, const protocol::Message& /*msg*/) {
            if (!session->is_logged_in(conn)) {
                protocol::Message reply(Cmd::LOGOUT, "not logged in");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            std::string pid = session->get_player_id(conn);
            session->logout(conn);
            protocol::Message reply(Cmd::LOGOUT, "logout ok: " + pid);
            service::ProtocolRouterComponent::send_message(conn, reply);
        });

    // HEARTBEAT：网关本地快速响应（无需转发后端）
    router->register_handler(Cmd::HEARTBEAT,
        [](const net::TcpConnectionPtr& conn, const protocol::Message& /*msg*/) {
            protocol::Message reply(Cmd::HEARTBEAT, "pong");
            service::ProtocolRouterComponent::send_message(conn, reply);
        });

    // ECHO、CHAT：转发到后端逻辑服
    router->register_handler(Cmd::ECHO,
        [forwarder](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
            forwarder->forward(conn, msg);
        });
    router->register_handler(Cmd::CHAT,
        [session, forwarder](const net::TcpConnectionPtr& conn,
                            const protocol::Message& msg) {
            if (!session->is_logged_in(conn)) {
                protocol::Message reply(Cmd::CHAT, "[Gateway] please login first");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            forwarder->forward(conn, msg);
        });

    svc.start();

    CHWELL_LOG_INFO("Gateway Server running on port " +
                                  std::to_string(gateway_port));
    CHWELL_LOG_INFO("Backend: " + backend_host + ":" +
                                  std::to_string(backend_port));
    CHWELL_LOG_INFO(
        "Local: LOGIN(10), LOGOUT(11), HEARTBEAT(3) | Forward: ECHO(1), CHAT(2)");

    // 阻塞主线程
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

    svc.stop();
    return 0;
}
