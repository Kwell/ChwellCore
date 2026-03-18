#include "chwell/game/game_components.h"
#include "chwell/service/protocol_router.h"
#include "chwell/service/session_manager.h"
#include "chwell/protocol/message.h"
#include "chwell/core/endian.h"
#include <cstring>

namespace chwell {
namespace game {

// ============================================
// 辅助函数：字符串编码/解码
// ============================================

static std::string encode_string(const std::string& str) {
    std::string result;
    // 长度（2字节，网络字节序）
    uint16_t len = core::host_to_net16(static_cast<uint16_t>(str.length()));
    result.append(reinterpret_cast<const char*>(&len), 2);
    // 内容
    result += str;
    return result;
}

static bool decode_string(const char* data, size_t size, size_t& offset, std::string& out) {
    if (offset + 2 > size) return false;

    // 读取长度
    uint16_t len_net;
    std::memcpy(&len_net, data + offset, 2);
    uint16_t len = core::net_to_host16(len_net);
    offset += 2;

    // 读取内容
    if (offset + len > size) return false;
    out.assign(data + offset, len);
    offset += len;

    return true;
}

static bool decode_int64(const char* data, size_t size, size_t& offset, int64_t& out) {
    if (offset + 8 > size) return false;

    // 读取 int64（小端）
    int64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value |= (static_cast<uint64_t>(static_cast<uint8_t>(data[offset + i])) << (i * 8));
    }
    out = value;
    offset += 8;

    return true;
}

static std::string encode_int64(int64_t value) {
    std::string result;
    result.resize(8);
    for (int i = 0; i < 8; i++) {
        result[i] = static_cast<char>((value >> (i * 8)) & 0xFF);
    }
    return result;
}

static bool decode_bool(const char* data, size_t size, size_t& offset, bool& out) {
    if (offset + 1 > size) return false;
    out = (data[offset] != 0);
    offset += 1;
    return true;
}

static std::string encode_bool(bool value) {
    return std::string(1, value ? '\x01' : '\x00');
}

// ============================================
// LoginComponent
// ============================================

void LoginComponent::on_register(service::Service& svc) {
    service_ = &svc;

    auto* router = svc.get_component<service::ProtocolRouterComponent>();
    if (router) {
        router->register_handler(cmd::C2S_LOGIN,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                this->handle_login(conn, msg.body);
            });
        CHWELL_LOG_INFO("LoginComponent registered handler for C2S_LOGIN");
    } else {
        CHWELL_LOG_WARN("ProtocolRouterComponent not found");
    }
}

void LoginComponent::handle_login(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    // 解析: [player_id_len][player_id][token_len][token]
    const char* ptr = data.data();
    size_t size = data.size();
    size_t offset = 0;

    std::string player_id;
    std::string token;

    if (!decode_string(ptr, size, offset, player_id)) {
        CHWELL_LOG_ERROR("Failed to decode player_id");
        send_login_response(conn, false, "Invalid player_id");
        return;
    }

    if (!decode_string(ptr, size, offset, token)) {
        CHWELL_LOG_ERROR("Failed to decode token");
        send_login_response(conn, false, "Invalid token");
        return;
    }

    CHWELL_LOG_INFO("Login request: player_id=" + player_id + ", token=" + token);

    // 简单验证
    if (player_id.empty()) {
        send_login_response(conn, false, "Player ID cannot be empty");
        return;
    }

    // 获取 SessionManager 并登录
    if (service_) {
        auto* session_mgr = service_->get_component<service::SessionManager>();
        if (session_mgr) {
            session_mgr->login(conn, player_id);
        }
    }

    CHWELL_LOG_INFO("Player logged in: " + player_id);

    // 发送登录成功响应
    send_login_response(conn, true, "Login success");
}

void LoginComponent::send_login_response(const net::TcpConnectionPtr& conn, bool ok, const std::string& message) {
    // 编码: [ok][message_len][message]
    std::string body;
    body += encode_bool(ok);
    body += encode_string(message);

    protocol::Message msg(cmd::S2C_LOGIN, std::vector<char>(body.begin(), body.end()));
    service::ProtocolRouterComponent::send_message(conn, msg);
}

// ============================================
// ChatComponent
// ============================================

void ChatComponent::on_register(service::Service& svc) {
    service_ = &svc;

    auto* router = svc.get_component<service::ProtocolRouterComponent>();
    if (router) {
        router->register_handler(cmd::C2S_CHAT,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                this->handle_chat(conn, msg.body);
            });
        CHWELL_LOG_INFO("ChatComponent registered handler for C2S_CHAT");
    }
}

void ChatComponent::handle_chat(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    // 解析: [room_id_len][room_id][content_len][content]
    const char* ptr = data.data();
    size_t size = data.size();
    size_t offset = 0;

    std::string room_id;
    std::string content;

    if (!decode_string(ptr, size, offset, room_id)) {
        CHWELL_LOG_ERROR("Failed to decode room_id");
        send_error_response(conn, error_code::INVALID_REQUEST, "Failed to decode room_id");
        return;
    }

    if (!decode_string(ptr, size, offset, content)) {
        CHWELL_LOG_ERROR("Failed to decode content");
        send_error_response(conn, error_code::INVALID_REQUEST, "Failed to decode content");
        return;
    }

    CHWELL_LOG_INFO("Chat message: room_id=" + room_id + ", content=" + content);

    // 获取玩家ID（从 SessionManager）
    std::string player_id = "unknown";
    if (service_) {
        auto* session_mgr = service_->get_component<service::SessionManager>();
        if (session_mgr) {
            player_id = session_mgr->get_player_id(conn);
            if (player_id.empty()) {
                // 未登录
                send_error_response(conn, error_code::NOT_LOGGED_IN, "Please login first");
                return;
            }
        }
    }

    // 广播聊天消息
    broadcast_chat(room_id, player_id, content);
}

void ChatComponent::broadcast_chat(const std::string& room_id, const std::string& from_player_id, const std::string& content) {
    // 获取 RoomComponent
    if (!service_) {
        CHWELL_LOG_WARN("Service not set, cannot broadcast chat");
        return;
    }

    auto* room_comp = service_->get_component<game::RoomComponent>();
    if (!room_comp) {
        CHWELL_LOG_WARN("RoomComponent not found, cannot broadcast chat");
        return;
    }

    // 获取房间内所有连接
    auto connections = room_comp->get_connections_in_room(room_id);

    // 广播聊天消息
    for (const auto& conn : connections) {
        send_chat_message(conn, from_player_id, content);
    }

    CHWELL_LOG_INFO("Broadcast chat to room " + room_id + ": " + from_player_id + " -> " + content + " (" + std::to_string(connections.size()) + " players)");
}

void ChatComponent::send_chat_message(const net::TcpConnectionPtr& conn, const std::string& from_player_id, const std::string& content) {
    // 编码: [from_player_id_len][from_player_id][content_len][content]
    std::string body;
    body += encode_string(from_player_id);
    body += encode_string(content);

    protocol::Message msg(cmd::S2C_CHAT, std::vector<char>(body.begin(), body.end()));
    service::ProtocolRouterComponent::send_message(conn, msg);
}

// ============================================
// RoomComponent
// ============================================

void RoomComponent::on_register(service::Service& svc) {
    service_ = &svc;

    auto* router = svc.get_component<service::ProtocolRouterComponent>();
    if (router) {
        router->register_handler(cmd::C2S_JOIN_ROOM,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                this->handle_join_room(conn, msg.body);
            });
        CHWELL_LOG_INFO("RoomComponent registered handler for C2S_JOIN_ROOM");
    }
}

void RoomComponent::handle_join_room(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    // 解析: [room_id_len][room_id]
    const char* ptr = data.data();
    size_t size = data.size();
    size_t offset = 0;

    std::string room_id;

    if (!decode_string(ptr, size, offset, room_id)) {
        CHWELL_LOG_ERROR("Failed to decode room_id");
        send_join_room_response(conn, false, "Invalid room_id", "");
        return;
    }

    CHWELL_LOG_INFO("Join room request: room_id=" + room_id);

    // 加入房间
    join_room(conn, room_id);

    // 更新 SessionManager
    if (service_) {
        auto* session_mgr = service_->get_component<service::SessionManager>();
        if (session_mgr) {
            session_mgr->join_room(conn, room_id);
        }
    }

    // 发送加入房间成功响应
    send_join_room_response(conn, true, "Join room success", room_id);
}

void RoomComponent::join_room(const net::TcpConnectionPtr& conn, const std::string& room_id) {
    // 查找或创建房间
    auto it = rooms_.find(room_id);
    std::shared_ptr<Room> room;

    if (it == rooms_.end()) {
        // 创建新房间
        room = std::make_shared<Room>();
        room->room_id = room_id;
        rooms_[room_id] = room;
        CHWELL_LOG_INFO("Created new room: " + room_id);
    } else {
        room = it->second;
    }

    // 添加连接到房间
    room->connections.insert(conn.get());

    // 保存连接映射
    connections_map_[conn.get()] = conn;

    CHWELL_LOG_INFO("Connection joined room: " + room_id);
}

void RoomComponent::leave_room(const net::TcpConnectionPtr& conn) {
    // 更新 SessionManager
    if (service_) {
        auto* session_mgr = service_->get_component<service::SessionManager>();
        if (session_mgr) {
            session_mgr->leave_room(conn);
        }
    }

    // 从所有房间中移除连接
    for (auto& pair : rooms_) {
        auto& room = pair.second;
        room->connections.erase(conn.get());
    }

    // 清理连接映射
    connections_map_.erase(conn.get());

    // 清理空房间
    auto it = rooms_.begin();
    while (it != rooms_.end()) {
        if (it->second->connections.empty()) {
            CHWELL_LOG_INFO("Room deleted: " + it->first);
            it = rooms_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<net::TcpConnectionPtr> RoomComponent::get_connections_in_room(const std::string& room_id) {
    std::vector<net::TcpConnectionPtr> result;

    auto it = rooms_.find(room_id);
    if (it != rooms_.end()) {
        auto& room = it->second;
        for (auto* raw_ptr : room->connections) {
            // 从连接映射中获取 shared_ptr
            auto map_it = connections_map_.find(raw_ptr);
            if (map_it != connections_map_.end()) {
                result.push_back(map_it->second);
            }
        }
    }

    return result;
}

void RoomComponent::on_disconnect(const net::TcpConnectionPtr& conn) {
    // 自动离开房间
    leave_room(conn);
}

void RoomComponent::send_join_room_response(const net::TcpConnectionPtr& conn, bool ok, const std::string& message, const std::string& room_id) {
    // 编码: [ok][message_len][message][room_id_len][room_id]
    std::string body;
    body += encode_bool(ok);
    body += encode_string(message);
    body += encode_string(room_id);

    protocol::Message msg(cmd::S2C_JOIN_ROOM, std::vector<char>(body.begin(), body.end()));
    service::ProtocolRouterComponent::send_message(conn, msg);
}

// ============================================
// HeartbeatComponent
// ============================================

void HeartbeatComponent::on_register(service::Service& svc) {
    auto* router = svc.get_component<service::ProtocolRouterComponent>();
    if (router) {
        router->register_handler(cmd::C2S_HEARTBEAT,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                this->handle_heartbeat(conn, msg.body);
            });
        CHWELL_LOG_INFO("HeartbeatComponent registered handler for C2S_HEARTBEAT");
    }
}

void HeartbeatComponent::handle_heartbeat(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    // 解析: [timestamp_ms]
    const char* ptr = data.data();
    size_t size = data.size();
    size_t offset = 0;

    int64_t timestamp_ms = 0;
    if (!decode_int64(ptr, size, offset, timestamp_ms)) {
        CHWELL_LOG_ERROR("Failed to decode heartbeat timestamp");
        return;
    }

    // 回应心跳
    send_heartbeat_response(conn, timestamp_ms);
}

void HeartbeatComponent::send_heartbeat_response(const net::TcpConnectionPtr& conn, int64_t timestamp_ms) {
    // 编码: [timestamp_ms]
    std::string body;
    body += encode_int64(timestamp_ms);

    protocol::Message msg(cmd::S2C_HEARTBEAT, std::vector<char>(body.begin(), body.end()));
    service::ProtocolRouterComponent::send_message(conn, msg);
}

// ============================================
// 辅助函数：发送错误响应
// ============================================

void send_error_response(const net::TcpConnectionPtr& conn, uint16_t error_code, const std::string& message) {
    // 编码: [error_code(2 bytes)][message_len][message]
    std::string body;

    // error_code（2字节，网络字节序）
    uint16_t code_net = core::host_to_net16(error_code);
    body.append(reinterpret_cast<const char*>(&code_net), 2);

    // message
    body += encode_string(message);

    protocol::Message msg(cmd::S2C_ERROR, std::vector<char>(body.begin(), body.end()));
    service::ProtocolRouterComponent::send_message(conn, msg);

    CHWELL_LOG_WARN("Send error response: code=" + std::to_string(error_code) + ", message=" + message);
}

} // namespace game
} // namespace chwell
