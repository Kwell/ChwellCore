#include "chwell/game/player_move.h"
#include "chwell/game/game_components.h"
#include "chwell/service/protocol_router.h"
#include "chwell/service/session_manager.h"
#include "chwell/protocol/message.h"
#include "chwell/core/endian.h"
#include <cstring>
#include <unordered_map>

namespace chwell {
namespace game {

// ============================================
// 辅助函数：float 编码/解码
// ============================================

static std::string encode_float(float value) {
    std::string result;
    result.resize(4);
    std::memcpy(result.data(), &value, 4);
    return result;
}

static bool decode_float(const char* data, size_t size, size_t& offset, float& out) {
    if (offset + 4 > size) return false;
    std::memcpy(&out, data + offset, 4);
    offset += 4;
    return true;
}

static std::string encode_string(const std::string& str) {
    std::string result;
    uint16_t len = core::host_to_net16(static_cast<uint16_t>(str.length()));
    result.append(reinterpret_cast<const char*>(&len), 2);
    result += str;
    return result;
}

static bool decode_string(const char* data, size_t size, size_t& offset, std::string& out) {
    if (offset + 2 > size) return false;
    uint16_t len_net;
    std::memcpy(&len_net, data + offset, 2);
    uint16_t len = core::net_to_host16(len_net);
    offset += 2;
    if (offset + len > size) return false;
    out.assign(data + offset, len);
    offset += len;
    return true;
}

// ============================================
// PlayerMoveComponent 实现
// ============================================

void PlayerMoveComponent::on_register(service::Service& svc) {
    service_ = &svc;

    auto* router = svc.get_component<service::ProtocolRouterComponent>();
    if (router) {
        router->register_handler(move_cmd::C2S_PLAYER_MOVE,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                this->handle_player_move(conn, msg.body);
            });
        CHWELL_LOG_INFO("PlayerMoveComponent registered handler for C2S_PLAYER_MOVE");
    }
}

void PlayerMoveComponent::handle_player_move(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    // 解析: [x(4 bytes)][y(4 bytes)][z(4 bytes)]
    const char* ptr = data.data();
    size_t size = data.size();
    size_t offset = 0;

    float x, y, z;
    if (!decode_float(ptr, size, offset, x)) {
        CHWELL_LOG_ERROR("Failed to decode x");
        return;
    }
    if (!decode_float(ptr, size, offset, y)) {
        CHWELL_LOG_ERROR("Failed to decode y");
        return;
    }
    if (!decode_float(ptr, size, offset, z)) {
        CHWELL_LOG_ERROR("Failed to decode z");
        return;
    }

    // 获取玩家ID
    std::string player_id = get_player_id(conn);
    if (player_id.empty()) {
        CHWELL_LOG_ERROR("Player not logged in");
        return;
    }

    CHWELL_LOG_INFO("Player move: player_id=" + player_id +
                    ", x=" + std::to_string(x) +
                    ", y=" + std::to_string(y) +
                    ", z=" + std::to_string(z));

    // 更新玩家位置
    PlayerPosition pos(x, y, z);
    update_player_position(player_id, pos);

    // 广播玩家位置
    std::string room_id = get_room_id(conn);
    if (!room_id.empty()) {
        broadcast_player_position(room_id, player_id, pos);
    }
}

void PlayerMoveComponent::broadcast_player_position(const std::string& room_id, const std::string& player_id, const PlayerPosition& pos) {
    // 获取 RoomComponent
    if (!service_) {
        CHWELL_LOG_WARN("Service not set, cannot broadcast player position");
        return;
    }

    auto* room_comp = service_->get_component<game::RoomComponent>();
    if (!room_comp) {
        CHWELL_LOG_WARN("RoomComponent not found, cannot broadcast player position");
        return;
    }

    // 获取房间内所有连接
    auto connections = room_comp->get_connections_in_room(room_id);

    // 广播玩家位置
    for (const auto& conn : connections) {
        send_player_position(conn, player_id, pos);
    }

    CHWELL_LOG_INFO("Broadcast player position to room " + room_id + ": " + player_id + " -> (" +
                    std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " + std::to_string(pos.z) +
                    ") (" + std::to_string(connections.size()) + " players)");
}

void PlayerMoveComponent::send_player_position(const net::TcpConnectionPtr& conn, const std::string& player_id, const PlayerPosition& pos) {
    // 编码: [player_id_len][player_id][x(4 bytes)][y(4 bytes)][z(4 bytes)]
    std::string body;

    // player_id
    body += encode_string(player_id);

    // x, y, z
    body += encode_float(pos.x);
    body += encode_float(pos.y);
    body += encode_float(pos.z);

    protocol::Message msg(move_cmd::S2C_PLAYER_POS, std::vector<char>(body.begin(), body.end()));
    service::ProtocolRouterComponent::send_message(conn, msg);
}

void PlayerMoveComponent::update_player_position(const std::string& player_id, const PlayerPosition& pos) {
    player_positions_[player_id] = pos;
}

bool PlayerMoveComponent::get_player_position(const std::string& player_id, PlayerPosition& out) {
    auto it = player_positions_.find(player_id);
    if (it != player_positions_.end()) {
        out = it->second;
        return true;
    }
    return false;
}

void PlayerMoveComponent::on_disconnect(const net::TcpConnectionPtr& conn) {
    // 获取玩家ID
    std::string player_id = get_player_id(conn);
    if (!player_id.empty()) {
        // 清理玩家位置
        player_positions_.erase(player_id);
        CHWELL_LOG_INFO("Player position removed: " + player_id);
    }
}

std::string PlayerMoveComponent::get_room_id(const net::TcpConnectionPtr& conn) {
    if (!service_) {
        return "";
    }

    auto* session_mgr = service_->get_component<service::SessionManager>();
    if (session_mgr) {
        return session_mgr->get_room_id(conn);
    }

    return "";
}

std::string PlayerMoveComponent::get_player_id(const net::TcpConnectionPtr& conn) {
    if (!service_) {
        return "";
    }

    auto* session_mgr = service_->get_component<service::SessionManager>();
    if (session_mgr) {
        return session_mgr->get_player_id(conn);
    }

    return "";
}

} // namespace game
} // namespace chwell
