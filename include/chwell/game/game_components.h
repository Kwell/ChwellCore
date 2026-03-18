#pragma once

#include "chwell/service/component.h"
#include "chwell/service/service.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/core/logger.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <memory>

namespace chwell {
namespace game {

// 游戏协议命令字
namespace cmd {
    const uint16_t C2S_LOGIN     = 0x0001;
    const uint16_t S2C_LOGIN     = 0x0002;
    const uint16_t C2S_CHAT      = 0x0003;
    const uint16_t S2C_CHAT      = 0x0004;
    const uint16_t C2S_HEARTBEAT = 0x0005;
    const uint16_t S2C_HEARTBEAT = 0x0006;
    const uint16_t C2S_JOIN_ROOM = 0x0007;
    const uint16_t S2C_JOIN_ROOM = 0x0008;
    const uint16_t S2C_ERROR     = 0x00FF;
}

// 错误码
namespace error_code {
    const uint16_t SUCCESS               = 0;
    const uint16_t INVALID_REQUEST       = 1;
    const uint16_t INVALID_PLAYER_ID      = 2;
    const uint16_t INVALID_TOKEN         = 3;
    const uint16_t NOT_LOGGED_IN         = 4;
    const uint16_t ROOM_NOT_FOUND        = 5;
    const uint16_t ALREADY_IN_ROOM       = 6;
    const uint16_t INTERNAL_ERROR        = 999;
}

// 登录组件
class LoginComponent : public service::Component {
public:
    virtual std::string name() const override { return "LoginComponent"; }

    // 注册协议处理器
    virtual void on_register(service::Service& svc) override;

    // 处理登录请求
    void handle_login(const net::TcpConnectionPtr& conn, const std::vector<char>& data);

    // 发送登录响应
    void send_login_response(const net::TcpConnectionPtr& conn, bool ok, const std::string& message);

private:
    service::Service* service_ = nullptr;
};

// 聊天组件
class ChatComponent : public service::Component {
public:
    virtual std::string name() const override { return "ChatComponent"; }

    // 注册协议处理器
    virtual void on_register(service::Service& svc) override;

    // 处理聊天请求
    void handle_chat(const net::TcpConnectionPtr& conn, const std::vector<char>& data);

    // 广播聊天消息到房间
    void broadcast_chat(const std::string& room_id, const std::string& from_player_id, const std::string& content);

    // 发送聊天消息
    void send_chat_message(const net::TcpConnectionPtr& conn, const std::string& from_player_id, const std::string& content);

private:
    service::Service* service_ = nullptr;
};

// 房间组件
class RoomComponent : public service::Component {
public:
    virtual std::string name() const override { return "RoomComponent"; }

    // 注册协议处理器
    virtual void on_register(service::Service& svc) override;

    // 处理加入房间请求
    void handle_join_room(const net::TcpConnectionPtr& conn, const std::vector<char>& data);

    // 加入房间
    void join_room(const net::TcpConnectionPtr& conn, const std::string& room_id);

    // 离开房间
    void leave_room(const net::TcpConnectionPtr& conn);

    // 获取房间内的连接
    std::vector<net::TcpConnectionPtr> get_connections_in_room(const std::string& room_id);

    // 连接断开时自动离开房间
    virtual void on_disconnect(const net::TcpConnectionPtr& conn) override;

    // 发送加入房间响应
    void send_join_room_response(const net::TcpConnectionPtr& conn, bool ok, const std::string& message, const std::string& room_id);

private:
    service::Service* service_ = nullptr;

private:
    struct Room {
        std::string room_id;
        std::unordered_set<net::TcpConnection*> connections;
    };

    std::unordered_map<std::string, std::shared_ptr<Room>> rooms_;

    // 连接映射：raw_ptr -> shared_ptr
    std::unordered_map<net::TcpConnection*, net::TcpConnectionPtr> connections_map_;
};

// 心跳组件
class HeartbeatComponent : public service::Component {
public:
    virtual std::string name() const override { return "HeartbeatComponent"; }

    // 注册协议处理器
    virtual void on_register(service::Service& svc) override;

    // 处理心跳请求
    void handle_heartbeat(const net::TcpConnectionPtr& conn, const std::vector<char>& data);

    // 发送心跳响应
    void send_heartbeat_response(const net::TcpConnectionPtr& conn, int64_t timestamp_ms);

private:
    // 心跳超时时间（秒）
    static const int HEARTBEAT_TIMEOUT = 60;
};

// 辅助函数：发送错误响应（声明）
void send_error_response(const net::TcpConnectionPtr& conn, uint16_t error_code, const std::string& message);

} // namespace game
} // namespace chwell
