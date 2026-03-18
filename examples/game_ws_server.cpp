#include <csignal>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include <ctime>

#include "chwell/core/logger.h"
#include "chwell/core/config.h"
#include "chwell/service/service.h"
#include "chwell/net/ws_server.h"

using namespace chwell;

// ============================================
// Protobuf 编码/解码工具
// ============================================

// 读取 varint32
static uint32_t readVarint32(const char* data, size_t& offset, size_t max_len) {
    uint32_t result = 0;
    int shift = 0;
    uint8_t byte;

    while (offset < max_len) {
        byte = static_cast<uint8_t>(data[offset++]);
        result |= (byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
        if (shift >= 35) {
            throw std::runtime_error("Invalid varint32");
        }
    }

    return result;
}

// 写入 varint32
static void writeVarint32(uint32_t value, std::string& out) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

// 编码带类型前缀的消息: [type_len][type_name][content_len][content]
static std::string encodeMessageWithType(const std::string& typeName, const std::string& content) {
    std::string result;

    // 类型名称长度 (varint32)
    writeVarint32(typeName.length(), result);

    // 类型名称
    result += typeName;

    // 内容长度 (varint32)
    writeVarint32(content.length(), result);

    // 内容
    result += content;

    return result;
}

// 解码带类型前缀的消息
static bool decodeMessageWithType(const std::string& data, std::string& typeName, std::string& content) {
    size_t offset = 0;

    try {
        // 读取类型名称长度
        uint32_t typeLen = readVarint32(data.data(), offset, data.length());

        if (offset + typeLen > data.length()) {
            return false;
        }

        // 读取类型名称
        typeName = data.substr(offset, typeLen);
        offset += typeLen;

        // 读取内容长度
        uint32_t contentLen = readVarint32(data.data(), offset, data.length());

        if (offset + contentLen > data.length()) {
            return false;
        }

        // 读取内容
        content = data.substr(offset, contentLen);

        return true;
    } catch (const std::exception& e) {
        CHWELL_LOG_ERROR("Failed to decode message: " + std::string(e.what()));
        return false;
    }
}

// ============================================
// 简单的 Protobuf 编码 (为了演示，简化实现)
// 实际项目应该使用真正的 Protobuf 库
// ============================================

// 编码 C2S_Login
static std::string encodeC2S_Login(const std::string& player_id, const std::string& token) {
    std::string result;

    // field 1: string player_id (tag 0x0A)
    result += '\x0A';
    writeVarint32(player_id.length(), result);
    result += player_id;

    // field 2: string token (tag 0x12)
    result += '\x12';
    writeVarint32(token.length(), result);
    result += token;

    return result;
}

// 编码 C2S_Chat
static std::string encodeC2S_Chat(const std::string& room_id, const std::string& content) {
    std::string result;

    // field 1: string room_id (tag 0x0A)
    result += '\x0A';
    writeVarint32(room_id.length(), result);
    result += room_id;

    // field 2: string content (tag 0x12)
    result += '\x12';
    writeVarint32(content.length(), result);
    result += content;

    return result;
}

// 编码 C2S_Heartbeat
static std::string encodeC2S_Heartbeat(int64_t timestamp_ms) {
    std::string result;

    // field 1: int64 timestamp_ms (tag 0x08)
    result += '\x08';
    // 写 int64 (小端)
    for (int i = 0; i < 8; i++) {
        result += static_cast<char>((timestamp_ms >> (i * 8)) & 0xFF);
    }

    return result;
}

// 编码 S2C_Login
static std::string encodeS2C_Login(bool ok, const std::string& message) {
    std::string result;

    // field 1: bool ok (tag 0x08)
    result += '\x08';
    result += ok ? '\x01' : '\x00';

    // field 2: string message (tag 0x12)
    result += '\x12';
    writeVarint32(message.length(), result);
    result += message;

    return result;
}

// 编码 S2C_Chat
static std::string encodeS2C_Chat(const std::string& from_player_id, const std::string& content) {
    std::string result;

    // field 1: string from_player_id (tag 0x0A)
    result += '\x0A';
    writeVarint32(from_player_id.length(), result);
    result += from_player_id;

    // field 2: string content (tag 0x12)
    result += '\x12';
    writeVarint32(content.length(), result);
    result += content;

    return result;
}

// 编码 S2C_Heartbeat
static std::string encodeS2C_Heartbeat(int64_t timestamp_ms) {
    std::string result;

    // field 1: int64 timestamp_ms (tag 0x08)
    result += '\x08';
    for (int i = 0; i < 8; i++) {
        result += static_cast<char>((timestamp_ms >> (i * 8)) & 0xFF);
    }

    return result;
}

// ============================================
// 游戏服务器组件
// ============================================

struct Player {
    std::string player_id;
    std::string room_id;
};

struct Room {
    std::string room_id;
    std::unordered_map<int, net::WsConnectionPtr> connections;
};

class GameWSComponent : public service::Component {
public:
    virtual std::string name() const override {
        return "GameWSComponent";
    }

    virtual void on_register(service::Service& /*svc*/) override {
        CHWELL_LOG_INFO("GameWSComponent registered");
    }

    // 处理断开连接（从 service::Component 继承）
    virtual void on_disconnect(const net::TcpConnectionPtr& /*conn*/) override {
        // TCP 连接断开时不需要处理（我们只处理 WebSocket）
    }

    // 处理 WebSocket 连接
    void on_connected(const net::WsConnectionPtr& conn) {
        int fd = conn->native_handle();
        connections_[fd] = conn;
        CHWELL_LOG_INFO("Client connected: fd=" + std::to_string(fd));
    }

    // 处理 WebSocket 断开连接
    void on_ws_disconnect(const net::WsConnectionPtr& conn) {
        int fd = conn->native_handle();
        CHWELL_LOG_INFO("Client disconnected: fd=" + std::to_string(fd));

        // 从连接列表移除
        connections_.erase(fd);

        // 移除玩家
        auto pit = players_.find(fd);
        if (pit != players_.end()) {
            const Player& player = pit->second;

            // 从房间移除
            auto rit = rooms_.find(player.room_id);
            if (rit != rooms_.end()) {
                Room& room = rit->second;
                room.connections.erase(fd);

                // 如果房间空了，删除房间
                if (room.connections.empty()) {
                    rooms_.erase(rit);
                    CHWELL_LOG_INFO("Room deleted: " + player.room_id);
                }
            }

            players_.erase(pit);
            CHWELL_LOG_INFO("Player removed: " + player.player_id);
        }
    }

    // 处理消息
    void on_message(const net::WsConnectionPtr& conn, const std::string& data) {
        int fd = conn->native_handle();
        std::string typeName, content;
        if (!decodeMessageWithType(data, typeName, content)) {
            CHWELL_LOG_ERROR("Failed to decode message");
            return;
        }

        CHWELL_LOG_INFO("Received message type: " + typeName);

        // 根据消息类型处理
        if (typeName == "C2S_Login") {
            handleLogin(conn, content);
        } else if (typeName == "C2S_Chat") {
            handleChat(conn, content);
        } else if (typeName == "C2S_Heartbeat") {
            handleHeartbeat(conn, content);
        } else {
            CHWELL_LOG_WARN("Unknown message type: " + typeName);
        }
    }

private:
    // 处理登录
    void handleLogin(const net::WsConnectionPtr& conn, const std::string& content) {
        int fd = conn->native_handle();

        // 解析 C2S_Login (简化实现)
        size_t offset = 1; // skip tag
        uint32_t player_id_len = readVarint32(content.data(), offset, content.length());
        std::string player_id = content.substr(offset, player_id_len);
        offset += player_id_len;

        offset++; // skip tag
        uint32_t token_len = readVarint32(content.data(), offset, content.length());
        std::string token = content.substr(offset, token_len);

        CHWELL_LOG_INFO("Login request: player_id=" + player_id + ", token=" + token);

        // 简单验证 (实际应该检查 token)
        if (player_id.empty()) {
            sendLogin(conn, false, "Invalid player_id");
            return;
        }

        // 创建玩家
        Player player;
        player.player_id = player_id;
        player.room_id = ""; // 还没有加入房间
        players_[fd] = player;

        CHWELL_LOG_INFO("Player logged in: " + player_id);

        // 发送登录成功响应
        sendLogin(conn, true, "Login success");
    }

    // 处理聊天
    void handleChat(const net::WsConnectionPtr& conn, const std::string& content) {
        int fd = conn->native_handle();

        // 检查是否已登录
        auto pit = players_.find(fd);
        if (pit == players_.end()) {
            CHWELL_LOG_WARN("Chat from unauthenticated connection: fd=" + std::to_string(fd));
            return;
        }

        // 解析 C2S_Chat
        size_t offset = 1; // skip tag
        uint32_t room_id_len = readVarint32(content.data(), offset, content.length());
        std::string room_id = content.substr(offset, room_id_len);
        offset += room_id_len;

        offset++; // skip tag
        uint32_t chat_len = readVarint32(content.data(), offset, content.length());
        std::string chat_content = content.substr(offset, chat_len);

        CHWELL_LOG_INFO("Chat message: room_id=" + room_id + ", content=" + chat_content);

        // 获取玩家信息
        Player& player = pit->second;
        player.room_id = room_id;

        // 广播消息到房间
        broadcastChat(room_id, player.player_id, chat_content);
    }

    // 处理心跳
    void handleHeartbeat(const net::WsConnectionPtr& conn, const std::string& content) {
        // 解析 C2S_Heartbeat
        int64_t timestamp_ms = 0;
        for (int i = 0; i < 8 && i < content.length(); i++) {
            timestamp_ms |= (static_cast<uint64_t>(static_cast<uint8_t>(content[i])) << (i * 8));
        }

        // 回应心跳
        sendHeartbeat(conn, timestamp_ms);
    }

    // 发送登录响应
    void sendLogin(const net::WsConnectionPtr& conn, bool ok, const std::string& message) {
        std::string content = encodeS2C_Login(ok, message);
        std::string message_with_type = encodeMessageWithType("S2C_Login", content);
        conn->send_text(message_with_type);
    }

    // 发送心跳响应
    void sendHeartbeat(const net::WsConnectionPtr& conn, int64_t timestamp_ms) {
        std::string content = encodeS2C_Heartbeat(timestamp_ms);
        std::string message_with_type = encodeMessageWithType("S2C_Heartbeat", content);
        conn->send_text(message_with_type);
    }

    // 广播聊天消息
    void broadcastChat(const std::string& room_id, const std::string& from_player_id, const std::string& content) {
        auto rit = rooms_.find(room_id);
        if (rit == rooms_.end()) {
            // 房间不存在，创建房间
            Room room;
            room.room_id = room_id;
            rooms_[room_id] = room;
            rit = rooms_.find(room_id);
        }

        Room& room = rit->second;

        // 编码 S2C_Chat
        std::string chat_content = encodeS2C_Chat(from_player_id, content);
        std::string message_with_type = encodeMessageWithType("S2C_Chat", chat_content);

        // 广播到房间内的所有连接
        for (const auto& [fd, conn] : room.connections) {
            conn->send_text(message_with_type);
        }

        CHWELL_LOG_INFO("Broadcasted chat to " + std::to_string(room.connections.size()) + " connections");
    }

private:
    std::unordered_map<int, net::WsConnectionPtr> connections_;
    std::unordered_map<int, Player> players_;
    std::unordered_map<std::string, Room> rooms_;
};

// ============================================
// 主函数
// ============================================

int main() {
    CHWELL_LOG_INFO("Starting Chwell WS Game Server...");

    core::Config cfg;
    cfg.load_from_file("server.conf");

    service::Service svc(static_cast<unsigned short>(cfg.listen_port()),
                         static_cast<std::size_t>(cfg.worker_threads()));

    auto game_component = svc.add_component<GameWSComponent>();

    // 创建 WebSocket 服务器
    auto ws_server = std::make_shared<net::WsServer>(svc.io_service(), cfg.listen_port());

    // 设置回调
    ws_server->set_connection_callback([game_component](const net::WsConnectionPtr& conn) {
        game_component->on_connected(conn);
    });

    ws_server->set_message_callback([game_component](const net::WsConnectionPtr& conn, const std::string& data) {
        game_component->on_message(conn, data);
    });

    ws_server->set_disconnect_callback([game_component](const net::WsConnectionPtr& conn) {
        game_component->on_ws_disconnect(conn);
    });

    svc.start();
    ws_server->start_accept();

    CHWELL_LOG_INFO("WS Game Server running on port " + std::to_string(cfg.listen_port()));

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

    ws_server->stop();
    svc.stop();

    CHWELL_LOG_INFO("WS Game Server stopped");
    return 0;
}
