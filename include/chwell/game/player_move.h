#pragma once

#include "chwell/service/component.h"
#include "chwell/service/service.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/core/logger.h"
#include <string>

namespace chwell {
namespace game {

// 移动协议命令字
namespace move_cmd {
    const uint16_t C2S_PLAYER_MOVE   = 0x0081;
    const uint16_t S2C_PLAYER_MOVE   = 0x0082;
    const uint16_t S2C_PLAYER_POS    = 0x0083;
    const uint16_t S2C_ERROR          = 0x00FF;
}

// 玩家位置
struct PlayerPosition {
    float x;
    float y;
    float z;

    PlayerPosition() : x(0), y(0), z(0) {}
    PlayerPosition(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

// 玩家移动组件
class PlayerMoveComponent : public service::Component {
public:
    PlayerMoveComponent() = default;

    virtual std::string name() const override { return "PlayerMoveComponent"; }

    // 注册协议处理器
    virtual void on_register(service::Service& svc) override;

    // 处理玩家移动请求
    void handle_player_move(const net::TcpConnectionPtr& conn, const std::vector<char>& data);

    // 广播玩家位置
    void broadcast_player_position(const std::string& room_id, const std::string& player_id, const PlayerPosition& pos);

    // 发送玩家位置
    void send_player_position(const net::TcpConnectionPtr& conn, const std::string& player_id, const PlayerPosition& pos);

    // 更新玩家位置
    void update_player_position(const std::string& player_id, const PlayerPosition& pos);

    // 获取玩家位置
    bool get_player_position(const std::string& player_id, PlayerPosition& out);

    // 连接断开时清理位置
    virtual void on_disconnect(const net::TcpConnectionPtr& conn) override;

private:
    // 获取房间ID
    std::string get_room_id(const net::TcpConnectionPtr& conn);

    // 获取玩家ID
    std::string get_player_id(const net::TcpConnectionPtr& conn);

    service::Service* service_ = nullptr;
    std::unordered_map<std::string, PlayerPosition> player_positions_; // player_id -> position
};

} // namespace game
} // namespace chwell
