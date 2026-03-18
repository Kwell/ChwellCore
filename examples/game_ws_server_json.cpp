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
// WebSocket 协议实现（简化版）
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

bool do_ws_handshake(int client_fd) {
  char buf[4096];
  ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
  if (n <= 0) return false;
  buf[n] = '\0';
  std::string headers(buf);

  string key;
  size_t pos = 0;
  for (;;) {
    size_t end = headers.find("\r\n", pos);
    if (end == string::npos) break;
    string line = headers.substr(pos, end - pos);
    pos = end + 2;
    if (line.empty()) break;

    if (line.find("Sec-WebSocket-Key:") == 0) {
      size_t start = line.find(':') + 1;
      while (start < line.size() && line[start] == ' ') start++;
      key = line.substr(start);
      while (!key.empty() && key.back() == '\r') key.pop_back();
      break;
    }
  }

  if (key.empty()) return false;

  std::string accept = ws_accept_key(key);
  std::string response =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

  ssize_t sent = send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
  return sent == static_cast<ssize_t>(response.size());
}

// 从 WebSocket 帧读取 payload（文本）
bool ws_read_text_frame(int fd, std::string& out) {
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
  if (payload_len > 4 * 1024 * 1024) return false; // 限制

  unsigned char mask_key[4];
  if (mask) {
    if (recv(fd, mask_key, 4, MSG_WAITALL) != 4) return false;
  }

  out.resize(payload_len);
  if (payload_len == 0) return true;

  ssize_t got = recv(fd, out.data(), out.size(), MSG_WAITALL);
  if (got != static_cast<ssize_t>(payload_len)) return false;

  if (mask) {
    for (size_t i = 0; i < out.size(); i++)
      out[i] = out[i] ^ mask_key[i % 4];
  }

  return true;
}

// 发送 WebSocket 文本帧
bool ws_write_text_frame(int fd, const char* data, size_t len) {
  std::vector<unsigned char> frame;
  frame.push_back(0x81); // FIN + text

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
// 简单游戏服务器（JSON 消息）
// ============================================

struct Player {
  string player_id;
  string room_id;
};

struct Room {
  string room_id;
  unordered_set<int> client_fds;
};

class GameServer {
public:
  GameServer(int port) : port_(port) {}

  void start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      throw runtime_error("Failed to create socket");
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      close(listen_fd_);
      throw runtime_error("Failed to bind socket");
    }

    if (listen(listen_fd_, SOMAXCONN) < 0) {
      close(listen_fd_);
      throw runtime_error("Failed to listen");
    }

    cout << "Game Server listening on port " << port_ << endl;

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
        cerr << "Failed to accept connection" << endl;
        continue;
      }

      cout << "New client connected: fd=" << client_fd << endl;

      // WebSocket 握手
      if (!do_ws_handshake(client_fd)) {
        cerr << "WebSocket handshake failed" << endl;
        close(client_fd);
        continue;
      }

      cout << "WebSocket handshake successful" << endl;

      // 创建客户端线程
      thread([this, client_fd]() {
        client_loop(client_fd);
      }).detach();
    }
  }

  void client_loop(int client_fd) {
    while (running_) {
      string payload;
      if (!ws_read_text_frame(client_fd, payload)) {
        cout << "Client disconnected: fd=" << client_fd << endl;
        on_disconnect(client_fd);
        close(client_fd);
        break;
      }

      cout << "Received message: " << payload << endl;

      // 简单的 JSON 消息处理
      if (payload.find("\"type\":\"login\"") != string::npos) {
        handleLogin(client_fd, payload);
      } else if (payload.find("\"type\":\"chat\"") != string::npos) {
        handleChat(client_fd, payload);
      } else {
        cerr << "Unknown message type" << endl;
      }
    }
  }

  void handleLogin(int client_fd, const string& payload) {
    // 简单的 JSON 解析
    size_t start = payload.find("\"playerId\"");
    if (start == string::npos) {
      cerr << "Failed to find playerId" << endl;
      return;
    }

    start = payload.find("\"", start + 11) + 1;
    size_t end = payload.find("\"", start);
    if (start == string::npos || end == string::npos || start > end) {
      cerr << "Failed to parse playerId" << endl;
      return;
    }

    string player_id = payload.substr(start, end - start);
    cout << "Login request: player_id=" << player_id << endl;

    // 创建玩家
    Player player;
    player.player_id = player_id;
    player.room_id = "";
    players_[client_fd] = player;

    cout << "Player logged in: " << player_id << endl;

    // 发送登录成功响应（JSON）
    string response = "{\"type\":\"login\",\"ok\":true,\"message\":\"Login success\"}";
    ws_write_text_frame(client_fd, response.data(), response.length());
  }

  void handleChat(int client_fd, const string& payload) {
    // 检查是否已登录
    auto pit = players_.find(client_fd);
    if (pit == players_.end()) {
      cerr << "Chat from unauthenticated connection: fd=" << client_fd << endl;
      return;
    }

    // 简单的 JSON 解析
    size_t start = payload.find("\"content\"");
    if (start == string::npos) {
      cerr << "Failed to find content" << endl;
      return;
    }

    start = payload.find("\"", start + 10) + 1;
    size_t end = payload.find("\"", start);
    if (start == string::npos || end == string::npos || start > end) {
      cerr << "Failed to parse content" << endl;
      return;
    }

    string content = payload.substr(start, end - start);
    cout << "Chat message: content=" << content << endl;

    // 获取玩家信息
    Player& player = pit->second;
    player.room_id = "main";

    // 广播消息到房间（JSON）
    string response = "{\"type\":\"chat\",\"fromPlayerId\":\"" + player.player_id + "\",\"content\":\"" + content + "\"}";
    broadcastChat("main", response);
  }

  void broadcastChat(const string& room_id, const string& message) {
    auto rit = rooms_.find(room_id);
    if (rit == rooms_.end()) {
      // 房间不存在，创建房间
      Room room;
      room.room_id = room_id;
      rooms_[room_id] = room;
      rit = rooms_.find(room_id);
    }

    Room& room = rit->second;

    // 广播到房间内的所有连接
    for (int fd : room.client_fds) {
      ws_write_text_frame(fd, message.data(), message.length());
    }

    cout << "Broadcasted chat to " << room.client_fds.size() << " connections" << endl;
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
          cout << "Room deleted: " << player.room_id << endl;
        }
      }

      players_.erase(pit);
      cout << "Player removed: " << player.player_id << endl;
    }
  }

private:
  int listen_fd_;
  int port_;
  bool running_ = true;

  unordered_map<int, Player> players_;
  unordered_map<string, Room> rooms_;
};

// ============================================
// 主函数
// ============================================

static volatile sig_atomic_t g_stop = 0;

int main() {
  cout << "Starting Chwell WS Game Server..." << endl;

  const int WS_PORT = 9080;

  // 信号处理
  signal(SIGTERM, [](int) { g_stop = 1; });
  signal(SIGINT, [](int) { g_stop = 1; });

  try {
    GameServer server(WS_PORT);
    server.start();
  } catch (const exception& e) {
    cerr << "Error: " << e.what() << endl;
    return 1;
  }

  cout << "Game Server stopped" << endl;
  return 0;
}
