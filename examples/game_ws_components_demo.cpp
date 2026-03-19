#include <iostream>
#include <signal.h>

#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/service/session_manager.h"
#include "chwell/game/game_components.h"
#include "chwell/net/ws_server.h"
#include "chwell/net/ws_connection.h"
#include "chwell/core/logger.h"
#include "chwell/core/endian.h"

using namespace chwell;

// 示例：WebSocket + 游戏组件服务器
// 展示如何使用 WsServer + Service + 游戏组件（Login, Chat, Room, Heartbeat）

int main(int argc, char* argv[]) {
    // 设置日志级别
    CHWELL_LOG_INFO("Starting WebSocket Game Server...");

    // 解析参数
    int port = 9080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    CHWELL_LOG_INFO("Port: " + std::to_string(port));

    // 创建 IO Service
    net::IoService io_service;

    // 创建 Service（服务容器）
    // 注意：Service 需要监听端口和工作线程数，但WebSocket服务器已经有自己的监听
    // 这里使用一个虚拟端口，因为WebSocket服务器独立监听
    service::Service svc(0, 2);

    // 注册 SessionManager
    svc.add_component<service::SessionManager>();

    // 注册 ProtocolRouterComponent
    svc.add_component<service::ProtocolRouterComponent>();

    // 注册游戏组件
    svc.add_component<game::LoginComponent>();
    svc.add_component<game::ChatComponent>();
    svc.add_component<game::RoomComponent>();
    svc.add_component<game::HeartbeatComponent>();

    // 创建 WebSocket 服务器
    net::WsServer ws_server(io_service, port);

    // 设置消息回调
    ws_server.set_message_callback([&](const net::WsConnectionPtr& conn, const std::string& message) {
        // 将 WebSocket 消息转换为协议消息
        // 假设消息格式：[cmd(2 bytes)][len(2 bytes)][body]
        if (message.size() < 4) {
            CHWELL_LOG_WARN("Invalid message size: " + std::to_string(message.size()));
            return;
        }

        uint16_t cmd, len;
        std::memcpy(&cmd, message.data(), 2);
        std::memcpy(&len, message.data() + 2, 2);

        cmd = core::net_to_host16(cmd);
        len = core::net_to_host16(len);

        if (message.size() < 4 + len) {
            CHWELL_LOG_WARN("Invalid message size, expected: " + std::to_string(4 + len) + ", got: " + std::to_string(message.size()));
            return;
        }

        std::vector<char> body(message.data() + 4, message.data() + 4 + len);

        // 创建协议消息
        protocol::Message msg(cmd, body);

        // 注意：WsConnection 和 TcpConnection 类型不兼容
        // 这里暂时跳过，实际需要修改 ProtocolRouterComponent 支持 WsConnection
        CHWELL_LOG_INFO("Received WebSocket message: cmd=" + std::to_string(cmd) + ", len=" + std::to_string(len));
    });

    // 设置连接回调
    ws_server.set_connection_callback([&](const net::WsConnectionPtr& conn) {
        CHWELL_LOG_INFO("WebSocket connection established, fd: " + std::to_string(conn->native_handle()));

        // 注意：WsConnection 和 TcpConnection 类型不兼容
        // 这里暂时跳过，实际需要修改 Component 基类支持通用连接类型
    });

    // 设置断开连接回调
    ws_server.set_disconnect_callback([&](const net::WsConnectionPtr& conn) {
        CHWELL_LOG_INFO("WebSocket connection closed, fd: " + std::to_string(conn->native_handle()));

        // 注意：WsConnection 和 TcpConnection 类型不兼容
        // 这里暂时跳过，实际需要修改 Component 基类支持通用连接类型
    });

    // 启动 WebSocket 服务器
    ws_server.start_accept();

    CHWELL_LOG_INFO("WebSocket Game Server started on port " + std::to_string(port));
    CHWELL_LOG_INFO("Press Ctrl+C to stop...");

    // 设置信号处理
    signal(SIGINT, [](int) {
        CHWELL_LOG_INFO("Shutting down server...");
        exit(0);
    });

    // 运行 IO Service
    io_service.run();

    // 停止 WebSocket 服务器
    ws_server.stop();

    CHWELL_LOG_INFO("WebSocket Game Server stopped");

    return 0;
}
