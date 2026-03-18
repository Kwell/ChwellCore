#include <csignal>
#include <iostream>
#include <memory>

#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/service/session_manager.h"
#include "chwell/sync/frame_sync.h"
#include "chwell/sync/state_sync.h"

using namespace chwell;

static volatile sig_atomic_t g_stop = 0;

void signal_handler(int) {
    g_stop = 1;
}

int main(int argc, char* argv[]) {
    // 设置信号处理
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    std::cout << "Starting Chwell Sync Server..." << std::endl;
    std::cout << "Frame Sync + State Sync Demo" << std::endl;

    // 获取端口号
    unsigned short port = 9000;
    if (argc > 1) {
        port = static_cast<unsigned short>(std::atoi(argv[1]));
    }

    // 创建同步服务
    service::Service sync_service(port, 4);

    // 添加组件

    // 1. 协议路由组件
    auto* router = sync_service.add_component<service::ProtocolRouterComponent>();
    if (router) {
        std::cout << "[+] ProtocolRouterComponent added" << std::endl;
    }

    // 2. 会话管理组件
    auto* session_mgr = sync_service.add_component<service::SessionManager>();
    if (session_mgr) {
        std::cout << "[+] SessionManager added" << std::endl;
    }

    // 3. 帧同步组件（30 FPS）
    auto* frame_sync = sync_service.add_component<sync::FrameSyncComponent>(30);
    if (frame_sync) {
        std::cout << "[+] FrameSyncComponent added (30 FPS)" << std::endl;

        // 创建房间
        frame_sync->create_room("fps_room");
        frame_sync->create_room("moba_room");
    }

    // 4. 状态同步组件
    auto* state_sync = sync_service.add_component<sync::StateSyncComponent>();
    if (state_sync) {
        std::cout << "[+] StateSyncComponent added" << std::endl;

        // 创建房间
        state_sync->create_room("mmo_room");
        state_sync->create_room("rpg_room");
    }

    std::cout << std::endl;
    std::cout << "Sync Server initialized" << std::endl;
    std::cout << "Listening on port " << port << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << std::endl;

    std::cout << "Frame Sync Rooms:" << std::endl;
    std::cout << "  - fps_room (FPS game)" << std::endl;
    std::cout << "  - moba_room (MOBA game)" << std::endl;
    std::cout << std::endl;
    std::cout << "State Sync Rooms:" << std::endl;
    std::cout << "  - mmo_room (MMO game)" << std::endl;
    std::cout << "  - rpg_room (RPG game)" << std::endl;
    std::cout << std::endl;

    // 启动服务
    sync_service.start();

    // 主循环：等待停止信号
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << std::endl;
    std::cout << "Stopping Sync Server..." << std::endl;

    // 停止服务
    sync_service.stop();

    std::cout << "Sync Server stopped" << std::endl;

    return 0;
}
