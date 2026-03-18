#include <csignal>
#include <iostream>
#include <memory>

#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/service/session_manager.h"
#include "chwell/game/game_components.h"

using namespace chwell;

static volatile sig_atomic_t g_stop = 0;

void signal_handler(int) {
    g_stop = 1;
}

int main(int argc, char* argv[]) {
    // 设置信号处理
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    std::cout << "Starting Chwell Game Server..." << std::endl;
    std::cout << "Game Components Demo" << std::endl;

    // 获取端口号
    unsigned short port = 9000;
    if (argc > 1) {
        port = static_cast<unsigned short>(std::atoi(argv[1]));
    }

    // 创建游戏服务
    // 参数：监听端口、工作线程数
    service::Service game_service(port, 4);

    // 添加组件

    // 1. 协议路由组件（必需，负责解析协议并路由到处理器）
    auto* router = game_service.add_component<service::ProtocolRouterComponent>();
    if (router) {
        std::cout << "[+] ProtocolRouterComponent added" << std::endl;
    }

    // 2. 会话管理组件（管理玩家会话）
    auto* session_mgr = game_service.add_component<service::SessionManager>();
    if (session_mgr) {
        std::cout << "[+] SessionManager added" << std::endl;
    }

    // 3. 登录组件（处理登录请求）
    auto* login_comp = game_service.add_component<game::LoginComponent>();
    if (login_comp) {
        std::cout << "[+] LoginComponent added" << std::endl;
    }

    // 4. 聊天组件（处理聊天消息）
    auto* chat_comp = game_service.add_component<game::ChatComponent>();
    if (chat_comp) {
        std::cout << "[+] ChatComponent added" << std::endl;
    }

    // 5. 房间组件（管理房间）
    auto* room_comp = game_service.add_component<game::RoomComponent>();
    if (room_comp) {
        std::cout << "[+] RoomComponent added" << std::endl;
    }

    // 6. 心跳组件（处理心跳）
    auto* heartbeat_comp = game_service.add_component<game::HeartbeatComponent>();
    if (heartbeat_comp) {
        std::cout << "[+] HeartbeatComponent added" << std::endl;
    }

    std::cout << std::endl;
    std::cout << "Game Server initialized" << std::endl;
    std::cout << "Listening on port " << port << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << std::endl;

    // 启动服务
    game_service.start();

    // 主循环：等待停止信号
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << std::endl;
    std::cout << "Stopping Game Server..." << std::endl;

    // 停止服务
    game_service.stop();

    std::cout << "Game Server stopped" << std::endl;

    return 0;
}
