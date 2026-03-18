#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <openssl/sha.h>

using namespace std;

// ============================================
// WebSocket 协议实现
// ============================================

static const char* WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string base64_encode(const unsigned char* data, size_t len) {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);

    for (size_t i = 0; i < len; i += 3) {
        unsigned char c0 = data[i];
        unsigned char c1 = (i + 1 < len) ? data[i + 1] : 0;
        unsigned char c2 = (i + 2 < len) ? data[i + 2] : 0;

        out.push_back(b64[c0 >> 2]);
        out.push_back(b64[((c0 & 0x03) << 4) | (c1 >> 4)]);
        out.push_back(b64[((c1 & 0x0F) << 2) | (c2 >> 6)]);
        out.push_back(b64[c2 & 0x3F]);
    }

    // Padding
    while (out.size() % 4) {
        out.push_back('=');
    }

    return out;
}

std::string ws_accept_key(const std::string& key) {
    std::string s = key + WS_MAGIC;
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(s.data()), s.size(), hash);
    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

bool do_ws_handshake(int client_fd, std::string& key_out) {
    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return false;
    buf[n] = '\0';
    std::string headers(buf);
    key_out.clear();

    size_t pos = 0;
    for (;;) {
        size_t end = headers.find("\r\n", pos);
        if (end == std::string::npos) break;
        std::string line = headers.substr(pos, end - pos);
        pos = end + 2;
        if (line.empty()) break;

        if (line.find("Sec-WebSocket-Key:") == 0) {
            size_t start = line.find(':') + 1;
            while (start < line.size() && line[start] == ' ') start++;
            key_out = line.substr(start);
            while (!key_out.empty() && key_out.back() == '\r') key_out.pop_back();
            break;
        }
    }

    if (key_out.empty()) return false;

    std::string accept = ws_accept_key(key_out);
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

    ssize_t sent = send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(response.size());
}

// 从 WebSocket 帧读取 payload（单帧，二进制）
bool ws_read_frame(int fd, std::vector<char>& out) {
    unsigned char hdr[2];
    if (recv(fd, hdr, 2, MSG_WAITALL) != 2) return false;

    unsigned char op = hdr[0] & 0x0f;
    bool mask = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7f;

    if (payload_len == 126) {
        unsigned char ext[2];
        if (recv(fd, ext, 2, MSG_WAITALL) != 2) return false;
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        unsigned char ext[8];
        if (recv(fd, ext, 8, MSG_WAITALL) != 8) return false;
        payload_len = 0;
        for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
    }

    if (op == 8) return false; // close
    if (payload_len > 4 * 1024 * 1024) return false; // 限制最大消息大小

    unsigned char mask_key[4];
    if (mask) {
        if (recv(fd, mask_key, 4, MSG_WAITALL) != 4) return false;
    }

    out.resize(static_cast<size_t>(payload_len));
    if (payload_len == 0) return true;

    ssize_t got = recv(fd, out.data(), out.size(), MSG_WAITALL);
    if (got != static_cast<ssize_t>(payload_len)) return false;

    if (mask) {
        for (size_t i = 0; i < out.size(); i++)
            out[i] = out[i] ^ mask_key[i % 4];
    }

    return true;
}

// 发送 WebSocket 二进制帧
bool ws_write_frame(int fd, const char* data, size_t len) {
    std::vector<unsigned char> frame;
    frame.push_back(0x82); // binary

    if (len < 126) {
        frame.push_back(static_cast<unsigned char>(len));
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<unsigned char>(len >> 8));
        frame.push_back(static_cast<unsigned char>(len & 0xff));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--)
            frame.push_back(static_cast<unsigned char>((len >> (i * 8)) & 0xff));
    }

    frame.insert(frame.end(), data, data + len);
    ssize_t sent = send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(frame.size());
}

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

    // 类型长度 (varint32)
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
        std::cerr << "Failed to decode message: " << e.what() << std::endl;
        return false;
    }
}

// 简单的 Protobuf 编码（为了演示，简化实现）

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
// 游戏服务器逻辑
// ============================================

struct Player {
    std::string player_id;
    std::string room_id;
};

struct Room {
    std::string room_id;
    std::unordered_set<int> client_fds;
};

class GameServer {
public:
    GameServer(int port) : port_(port) {}

    void start() {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            throw std::runtime_error("Failed to create socket");
        }

        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(listen_fd_);
            throw std::runtime_error("Failed to bind socket");
        }

        if (listen(listen_fd_, SOMAXCONN) < 0) {
            close(listen_fd_);
            throw std::runtime_error("Failed to listen");
        }

        std::cout << "Game Server listening on port " << port_ << std::endl;

        accept_loop();
    }

private:
    void accept_loop() {
        while (running_) {
            struct pollfd pfd = { listen_fd_, POLLIN, 0 };
            int ret = poll(&pfd, 1, 1000);
            if (ret <= 0) continue;

            struct sockaddr_in client_addr = {};
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);

            if (client_fd < 0) {
                std::cerr << "Failed to accept connection" << std::endl;
                continue;
            }

            std::cout << "New client connected: fd=" << client_fd << std::endl;

            // WebSocket 握手
            std::string key;
            if (!do_ws_handshake(client_fd, key)) {
                std::cerr << "WebSocket handshake failed" << std::endl;
                close(client_fd);
                continue;
            }

            std::cout << "WebSocket handshake successful" << std::endl;

            // 创建客户端线程
            std::thread([this, client_fd]() {
                client_loop(client_fd);
            }).detach();
        }
    }

    void client_loop(int client_fd) {
        while (running_) {
            std::vector<char> payload;
            if (!ws_read_frame(client_fd, payload)) {
                std::cout << "Client disconnected: fd=" << client_fd << std::endl;
                on_disconnect(client_fd);
                close(client_fd);
                break;
            }

            // 解析消息
            std::string typeName, content;
            if (!decodeMessageWithType(std::string(payload.data(), payload.size()), typeName, content)) {
                std::cerr << "Failed to decode message" << std::endl;
                continue;
            }

            std::cout << "Received message type: " << typeName << std::endl;

            // 处理消息
            if (typeName == "C2S_Login") {
                handleLogin(client_fd, content);
            } else if (typeName == "C2S_Chat") {
                handleChat(client_fd, content);
            } else if (typeName == "C2S_Heartbeat") {
                handleHeartbeat(client_fd, content);
            } else {
                std::cerr << "Unknown message type: " << typeName << std::endl;
            }
        }
    }

    void handleLogin(int client_fd, const std::string& content) {
        // 解析 C2S_Login
        size_t offset = 1; // skip tag
        uint32_t player_id_len = readVarint32(content.data(), offset, content.length());
        std::string player_id = content.substr(offset, player_id_len);
        offset += player_id_len;

        offset++; // skip tag
        uint32_t token_len = readVarint32(content.data(), offset, content.length());
        std::string token = content.substr(offset, token_len);

        std::cout << "Login request: player_id=" << player_id << ", token=" << token << std::endl;

        // 简单验证
        if (player_id.empty()) {
            sendLogin(client_fd, false, "Invalid player_id");
            return;
        }

        // 创建玩家
        Player player;
        player.player_id = player_id;
        player.room_id = "";
        players_[client_fd] = player;

        std::cout << "Player logged in: " << player_id << std::endl;

        // 发送登录成功响应
        sendLogin(client_fd, true, "Login success");
    }

    void handleChat(int client_fd, const std::string& content) {
        // 检查是否已登录
        auto pit = players_.find(client_fd);
        if (pit == players_.end()) {
            std::cerr << "Chat from unauthenticated connection: fd=" << client_fd << std::endl;
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

        std::cout << "Chat message: room_id=" << room_id << ", content=" << chat_content << std::endl;

        // 获取玩家信息
        Player& player = pit->second;
        player.room_id = room_id;

        // 广播消息到房间
        broadcastChat(room_id, player.player_id, chat_content);
    }

    void handleHeartbeat(int client_fd, const std::string& content) {
        // 解析 C2S_Heartbeat
        int64_t timestamp_ms = 0;
        for (int i = 0; i < 8 && i < content.length(); i++) {
            timestamp_ms |= (static_cast<uint64_t>(static_cast<uint8_t>(content[i])) << (i * 8));
        }

        // 回应心跳
        sendHeartbeat(client_fd, timestamp_ms);
    }

    void sendLogin(int client_fd, bool ok, const std::string& message) {
        std::string content = encodeS2C_Login(ok, message);
        std::string message_with_type = encodeMessageWithType("S2C_Login", content);
        ws_write_frame(client_fd, message_with_type.data(), message_with_type.length());
    }

    void sendHeartbeat(int client_fd, int64_t timestamp_ms) {
        std::string content = encodeS2C_Heartbeat(timestamp_ms);
        std::string message_with_type = encodeMessageWithType("S2C_Heartbeat", content);
        ws_write_frame(client_fd, message_with_type.data(), message_with_type.length());
    }

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
        for (int fd : room.client_fds) {
            ws_write_frame(fd, message_with_type.data(), message_with_type.length());
        }

        std::cout << "Broadcasted chat to " << room.client_fds.size() << " connections" << std::endl;
    }

    void on_disconnect(int client_fd) {
        // 从玩家列表移除
        auto pit = players_.find(client_fd);
        if (pit != players_.end()) {
            const Player& player = pit->second;

            // 从房间移除
            auto rit = rooms_.find(player.room_id);
            if (rit != rooms_.end()) {
                Room& room = rit->second;
                room.client_fds.erase(client_fd);

                // 如果房间空了，删除房间
                if (room.client_fds.empty()) {
                    rooms_.erase(rit);
                    std::cout << "Room deleted: " << player.room_id << std::endl;
                }
            }

            players_.erase(pit);
            std::cout << "Player removed: " << player.player_id << std::endl;
        }
    }

private:
    int listen_fd_;
    int port_;
    bool running_ = true;

    std::unordered_map<int, Player> players_;
    std::unordered_map<std::string, Room> rooms_;
};

// ============================================
// 主函数
// ============================================

static volatile sig_atomic_t g_stop = 0;

int main() {
    std::cout << "Starting Chwell WS Game Server..." << std::endl;

    const int WS_PORT = 9080;

    // 信号处理
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });

    try {
        GameServer server(WS_PORT);
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Game Server stopped" << std::endl;
    return 0;
}
