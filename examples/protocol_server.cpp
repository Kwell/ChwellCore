#include <csignal>
#include <iostream>
#include <string>
#include <unistd.h>

#include "chwell/core/logger.h"
#include "chwell/core/config.h"
#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/service/session_component.h"
#include "chwell/protocol/message.h"

using namespace chwell;

// 定义一些命令号（实际项目中应该放在公共头文件中）
namespace Cmd {
    const std::uint16_t ECHO      = 1;
    const std::uint16_t CHAT      = 2;
    const std::uint16_t HEARTBEAT = 3;
    const std::uint16_t LOGIN     = 10;
    const std::uint16_t LOGOUT    = 11;
}

// Echo 业务组件：处理 ECHO 命令
class EchoHandlerComponent : public service::Component {
public:
    virtual std::string name() const override {
        return "EchoHandlerComponent";
    }

    virtual void on_register(service::Service& svc) override {
        // 获取协议路由组件（假设已经注册）
        // 注意：这里简化处理，实际应该通过 Service 获取已注册的组件
        // 为了演示，我们在主函数中手动设置
    }

    void handle_echo(const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
        std::string text(msg.body.begin(), msg.body.end());
        core::Logger::instance().info("EchoHandler received: " + text);

        // 回显消息
        protocol::Message reply(Cmd::ECHO, "Echo: " + text);
        service::ProtocolRouterComponent::send_message(conn, reply);
    }
};

// Chat 业务组件：处理 CHAT 命令
class ChatHandlerComponent : public service::Component {
public:
    virtual std::string name() const override {
        return "ChatHandlerComponent";
    }

    void handle_chat(const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
        std::string text(msg.body.begin(), msg.body.end());
        core::Logger::instance().info("ChatHandler received: " + text);

        // 广播聊天消息（这里简化处理，只回复发送者）
        protocol::Message reply(Cmd::CHAT, "[Server] " + text);
        service::ProtocolRouterComponent::send_message(conn, reply);
    }
};

int main() {
    core::Logger::instance().info("Starting Protocol Router Server...");

    core::Config cfg;
    cfg.load_from_file("server.conf");

    // 创建服务
    service::Service svc(static_cast<unsigned short>(cfg.listen_port()),
                         static_cast<std::size_t>(cfg.worker_threads()));

    // 注册协议路由组件（必须先注册，因为它负责解析协议）
    auto* router = svc.add_component<service::ProtocolRouterComponent>();

    // 注册会话组件
    auto* session = svc.add_component<service::SessionComponent>();

    // 创建业务组件
    auto* echo_handler = svc.add_component<EchoHandlerComponent>();
    auto* chat_handler = svc.add_component<ChatHandlerComponent>();

    // 注册命令处理器到路由组件
    router->register_handler(Cmd::ECHO,
        [echo_handler](const net::TcpConnectionPtr& conn,
                       const protocol::Message& msg) {
            echo_handler->handle_echo(conn, msg);
        });

    // 只有已登录玩家才能聊天
    router->register_handler(Cmd::CHAT,
        [chat_handler, session](const net::TcpConnectionPtr& conn,
                                const protocol::Message& msg) {
            if (!session->is_logged_in(conn)) {
                protocol::Message reply(Cmd::CHAT, "[Server] please login first");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            chat_handler->handle_chat(conn, msg);
        });

    // 登录：body 直接视为 player_id
    router->register_handler(Cmd::LOGIN,
        [session](const net::TcpConnectionPtr& conn,
                  const protocol::Message& msg) {
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

    // 登出
    router->register_handler(Cmd::LOGOUT,
        [session](const net::TcpConnectionPtr& conn,
                  const protocol::Message& /*msg*/) {
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

    // 心跳
    router->register_handler(Cmd::HEARTBEAT,
        [](const net::TcpConnectionPtr& conn,
           const protocol::Message& /*msg*/) {
            protocol::Message reply(Cmd::HEARTBEAT, "pong");
            service::ProtocolRouterComponent::send_message(conn, reply);
            core::Logger::instance().debug("Heartbeat received");
        });

    svc.start();

    core::Logger::instance().info("Protocol Router Server running on port " +
                                  std::to_string(cfg.listen_port()));
    core::Logger::instance().info(
        "Supported commands: ECHO(1), CHAT(2), HEARTBEAT(3), LOGIN(10), LOGOUT(11)");

    // 阻塞主线程：交互模式等待回车，Docker/后台模式等待 SIGTERM/SIGINT
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
