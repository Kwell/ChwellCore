#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

#include "chwell/core/logger.h"
#include "chwell/core/config.h"
#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/service/session_manager.h"
#include "chwell/gateway/gateway_forwarder.h"
#include "chwell/protocol/message.h"
#include "game_commands.h"

using namespace chwell;

int main() {
    CHWELL_LOG_INFO("Starting Game Gateway Server...");

    core::Config cfg;
    cfg.load_from_file("gateway.conf");

    int port = 9001;
    if (const char* env = std::getenv("GATEWAY_PORT")) {
        int p = std::atoi(env);
        if (p > 0) port = p;
    } else if (cfg.listen_port() > 0) {
        port = cfg.listen_port();
    }
    unsigned short gateway_port = static_cast<unsigned short>(port);

    std::string backend_host = "127.0.0.1";
    if (const char* env = std::getenv("BACKEND_HOST")) backend_host = env;
    unsigned short backend_port = 9000;
    if (const char* env = std::getenv("BACKEND_PORT")) {
        backend_port = static_cast<unsigned short>(std::atoi(env));
    }

    service::Service svc(gateway_port, static_cast<std::size_t>(cfg.worker_threads()));

    auto* router = svc.add_component<service::ProtocolRouterComponent>();
    auto* session = svc.add_component<service::SessionManager>();
    auto* forwarder = svc.add_component<gateway::GatewayForwarderComponent>(backend_host, backend_port);

    auto forward = [forwarder](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
        forwarder->forward(conn, msg);
    };

    // 本地处理
    router->register_handler(GameCmd::LOGIN,
        [session](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
            std::string player_id(msg.body.begin(), msg.body.end());
            if (player_id.empty()) {
                protocol::Message reply(GameCmd::LOGIN, "login failed: empty player_id");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            session->login(conn, player_id);
            protocol::Message reply(GameCmd::LOGIN, "login ok: " + player_id);
            service::ProtocolRouterComponent::send_message(conn, reply);
        });

    router->register_handler(GameCmd::LOGOUT,
        [session](const net::TcpConnectionPtr& conn, const protocol::Message&) {
            if (!session->is_logged_in(conn)) {
                protocol::Message reply(GameCmd::LOGOUT, "not logged in");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            std::string pid = session->get_player_id(conn);
            session->logout(conn);
            protocol::Message reply(GameCmd::LOGOUT, "logout ok: " + pid);
            service::ProtocolRouterComponent::send_message(conn, reply);
        });

    router->register_handler(GameCmd::HEARTBEAT,
        [](const net::TcpConnectionPtr& conn, const protocol::Message&) {
            protocol::Message reply(GameCmd::HEARTBEAT, "pong");
            service::ProtocolRouterComponent::send_message(conn, reply);
        });

    // 转发到游戏服
    router->register_handler(GameCmd::LIST_ROOMS, forward);
    router->register_handler(GameCmd::CREATE_ROOM, forward);
    router->register_handler(GameCmd::JOIN_ROOM, forward);
    router->register_handler(GameCmd::LEAVE_ROOM, forward);
    router->register_handler(GameCmd::ROOM_INFO, forward);
    router->register_handler(GameCmd::START_GAME, forward);
    router->register_handler(GameCmd::GAME_STATE, forward);
    router->register_handler(GameCmd::GAME_ACTION, forward);

    svc.start();

    CHWELL_LOG_INFO("Game Gateway on port " + std::to_string(gateway_port) + ", backend " + backend_host + ":" + std::to_string(backend_port));

    static volatile sig_atomic_t g_stop = 0;
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });

    if (isatty(STDIN_FILENO)) {
        std::cout << "Press ENTER to exit..." << std::endl;
        std::string line;
        std::getline(std::cin, line);
    } else {
        while (!g_stop) sleep(1);
    }

    svc.stop();
    return 0;
}
