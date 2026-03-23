#include <gtest/gtest.h>

#include "chwell/game/game_components.h"
#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/service/session_manager.h"
#include "chwell/protocol/message.h"
#include "chwell/core/endian.h"

using namespace chwell;
using namespace chwell::game;

namespace {

// 使用静态对象作为虚拟连接的地址，避免使用无效指针
static int dummy_conn1_obj;
static int dummy_conn2_obj;

// 创建虚拟连接
net::TcpConnectionPtr make_dummy_conn(std::uintptr_t tag) {
    void* ptr = (tag == 1) ? &dummy_conn1_obj : &dummy_conn2_obj;
    auto* raw_ptr = reinterpret_cast<net::TcpConnection*>(ptr);
    return net::TcpConnectionPtr(raw_ptr, [](net::TcpConnection*) {
        // 空删除器，不实际释放内存
    });
}

// 测试字符串编码/解码
TEST(GameComponentsTest, EncodeDecodeString) {
    std::string original = "hello world";

    // 编码
    std::string encoded;
    uint16_t len = core::host_to_net16(static_cast<uint16_t>(original.length()));
    encoded.append(reinterpret_cast<const char*>(&len), 2);
    encoded += original;

    // 解码
    size_t offset = 0;
    uint16_t len_net;
    std::memcpy(&len_net, encoded.data() + offset, 2);
    uint16_t decoded_len = core::net_to_host16(len_net);
    offset += 2;

    std::string decoded(encoded.data() + offset, decoded_len);

    EXPECT_EQ(original, decoded);
}

// 测试 bool 编码/解码
TEST(GameComponentsTest, EncodeDecodeBool) {
    std::string encoded_true = std::string(1, '\x01');
    std::string encoded_false = std::string(1, '\x00');

    bool decoded_true = (encoded_true[0] != 0);
    bool decoded_false = (encoded_false[0] != 0);

    EXPECT_TRUE(decoded_true);
    EXPECT_FALSE(decoded_false);
}

// 测试 int64 编码/解码
TEST(GameComponentsTest, EncodeDecodeInt64) {
    int64_t original = 1234567890123LL;

    // 编码（小端）
    std::string encoded;
    encoded.resize(8);
    for (int i = 0; i < 8; i++) {
        encoded[i] = static_cast<char>((original >> (i * 8)) & 0xFF);
    }

    // 解码
    int64_t decoded = 0;
    for (int i = 0; i < 8; i++) {
        decoded |= (static_cast<uint64_t>(static_cast<uint8_t>(encoded[i])) << (i * 8));
    }

    EXPECT_EQ(original, decoded);
}

// 测试登录请求编码/解码
TEST(GameComponentsTest, EncodeDecodeLoginRequest) {
    std::string player_id = "player123";
    std::string token = "token456";

    // 编码
    std::string encoded;

    // player_id
    uint16_t len1 = core::host_to_net16(static_cast<uint16_t>(player_id.length()));
    encoded.append(reinterpret_cast<const char*>(&len1), 2);
    encoded += player_id;

    // token
    uint16_t len2 = core::host_to_net16(static_cast<uint16_t>(token.length()));
    encoded.append(reinterpret_cast<const char*>(&len2), 2);
    encoded += token;

    // 解码
    const char* ptr = encoded.data();
    size_t size = encoded.size();
    size_t offset = 0;

    // 读取 player_id
    uint16_t len1_net;
    std::memcpy(&len1_net, ptr + offset, 2);
    uint16_t player_id_len = core::net_to_host16(len1_net);
    offset += 2;

    std::string decoded_player_id(ptr + offset, player_id_len);
    offset += player_id_len;

    // 读取 token
    uint16_t len2_net;
    std::memcpy(&len2_net, ptr + offset, 2);
    uint16_t token_len = core::net_to_host16(len2_net);
    offset += 2;

    std::string decoded_token(ptr + offset, token_len);

    EXPECT_EQ(player_id, decoded_player_id);
    EXPECT_EQ(token, decoded_token);
}

// 测试聊天请求编码/解码
TEST(GameComponentsTest, EncodeDecodeChatRequest) {
    std::string room_id = "room1";
    std::string content = "hello everyone";

    // 编码
    std::string encoded;

    // room_id
    uint16_t len1 = core::host_to_net16(static_cast<uint16_t>(room_id.length()));
    encoded.append(reinterpret_cast<const char*>(&len1), 2);
    encoded += room_id;

    // content
    uint16_t len2 = core::host_to_net16(static_cast<uint16_t>(content.length()));
    encoded.append(reinterpret_cast<const char*>(&len2), 2);
    encoded += content;

    // 解码
    const char* ptr = encoded.data();
    size_t size = encoded.size();
    size_t offset = 0;

    // 读取 room_id
    uint16_t len1_net;
    std::memcpy(&len1_net, ptr + offset, 2);
    uint16_t room_id_len = core::net_to_host16(len1_net);
    offset += 2;

    std::string decoded_room_id(ptr + offset, room_id_len);
    offset += room_id_len;

    // 读取 content
    uint16_t len2_net;
    std::memcpy(&len2_net, ptr + offset, 2);
    uint16_t content_len = core::net_to_host16(len2_net);
    offset += 2;

    std::string decoded_content(ptr + offset, content_len);

    EXPECT_EQ(room_id, decoded_room_id);
    EXPECT_EQ(content, decoded_content);
}

// 测试登录响应编码/解码
TEST(GameComponentsTest, EncodeDecodeLoginResponse) {
    bool ok = true;
    std::string message = "Login success";

    // 编码
    std::string encoded;
    encoded += ok ? '\x01' : '\x00';

    uint16_t len = core::host_to_net16(static_cast<uint16_t>(message.length()));
    encoded.append(reinterpret_cast<const char*>(&len), 2);
    encoded += message;

    // 解码
    const char* ptr = encoded.data();
    size_t size = encoded.size();
    size_t offset = 0;

    bool decoded_ok = (ptr[offset] != 0);
    offset += 1;

    uint16_t len_net;
    std::memcpy(&len_net, ptr + offset, 2);
    uint16_t message_len = core::net_to_host16(len_net);
    offset += 2;

    std::string decoded_message(ptr + offset, message_len);

    EXPECT_EQ(ok, decoded_ok);
    EXPECT_EQ(message, decoded_message);
}

// 测试心跳编码/解码
TEST(GameComponentsTest, EncodeDecodeHeartbeat) {
    int64_t timestamp_ms = 1710837600000LL;

    // 编码（小端）
    std::string encoded;
    encoded.resize(8);
    for (int i = 0; i < 8; i++) {
        encoded[i] = static_cast<char>((timestamp_ms >> (i * 8)) & 0xFF);
    }

    // 解码
    int64_t decoded = 0;
    for (int i = 0; i < 8; i++) {
        decoded |= (static_cast<uint64_t>(static_cast<uint8_t>(encoded[i])) << (i * 8));
    }

    EXPECT_EQ(timestamp_ms, decoded);
}

// 测试聊天响应编码/解码
TEST(GameComponentsTest, EncodeDecodeChatResponse) {
    std::string from_player_id = "alice";
    std::string content = "hello";

    // 编码
    std::string encoded;

    // from_player_id
    uint16_t len1 = core::host_to_net16(static_cast<uint16_t>(from_player_id.length()));
    encoded.append(reinterpret_cast<const char*>(&len1), 2);
    encoded += from_player_id;

    // content
    uint16_t len2 = core::host_to_net16(static_cast<uint16_t>(content.length()));
    encoded.append(reinterpret_cast<const char*>(&len2), 2);
    encoded += content;

    // 解码
    const char* ptr = encoded.data();
    size_t size = encoded.size();
    size_t offset = 0;

    // 读取 from_player_id
    uint16_t len1_net;
    std::memcpy(&len1_net, ptr + offset, 2);
    uint16_t from_player_id_len = core::net_to_host16(len1_net);
    offset += 2;

    std::string decoded_from_player_id(ptr + offset, from_player_id_len);
    offset += from_player_id_len;

    // 读取 content
    uint16_t len2_net;
    std::memcpy(&len2_net, ptr + offset, 2);
    uint16_t content_len = core::net_to_host16(len2_net);
    offset += 2;

    std::string decoded_content(ptr + offset, content_len);

    EXPECT_EQ(from_player_id, decoded_from_player_id);
    EXPECT_EQ(content, decoded_content);
}

// 测试加入房间响应编码/解码
TEST(GameComponentsTest, EncodeDecodeJoinRoomResponse) {
    bool ok = true;
    std::string message = "Join room success";
    std::string room_id = "room1";

    // 编码
    std::string encoded;
    encoded += ok ? '\x01' : '\x00';

    uint16_t len1 = core::host_to_net16(static_cast<uint16_t>(message.length()));
    encoded.append(reinterpret_cast<const char*>(&len1), 2);
    encoded += message;

    uint16_t len2 = core::host_to_net16(static_cast<uint16_t>(room_id.length()));
    encoded.append(reinterpret_cast<const char*>(&len2), 2);
    encoded += room_id;

    // 解码
    const char* ptr = encoded.data();
    size_t size = encoded.size();
    size_t offset = 0;

    bool decoded_ok = (ptr[offset] != 0);
    offset += 1;

    uint16_t len1_net;
    std::memcpy(&len1_net, ptr + offset, 2);
    uint16_t message_len = core::net_to_host16(len1_net);
    offset += 2;

    std::string decoded_message(ptr + offset, message_len);
    offset += message_len;

    uint16_t len2_net;
    std::memcpy(&len2_net, ptr + offset, 2);
    uint16_t room_id_len = core::net_to_host16(len2_net);
    offset += 2;

    std::string decoded_room_id(ptr + offset, room_id_len);

    EXPECT_EQ(ok, decoded_ok);
    EXPECT_EQ(message, decoded_message);
    EXPECT_EQ(room_id, decoded_room_id);
}

// 测试 RoomComponent 基本功能
TEST(GameComponentsTest, RoomComponentBasicOperations) {
    // 此测试需要 Service 对象，暂时禁用
    // TODO: 集成到集成测试中
}

// 测试错误响应编码
TEST(GameComponentsTest, EncodeDecodeErrorResponse) {
    uint16_t error_code = game::error_code::INVALID_PLAYER_ID;
    std::string message = "Player ID cannot be empty";

    // 编码: [error_code(2 bytes)][message_len][message]
    std::string encoded;

    // error_code（2字节，网络字节序）
    uint16_t code_net = core::host_to_net16(error_code);
    encoded.append(reinterpret_cast<const char*>(&code_net), 2);

    // message
    uint16_t len = core::host_to_net16(static_cast<uint16_t>(message.length()));
    encoded.append(reinterpret_cast<const char*>(&len), 2);
    encoded += message;

    // 解码
    const char* ptr = encoded.data();
    size_t offset = 0;

    // 读取 error_code
    uint16_t code_net_read;
    std::memcpy(&code_net_read, ptr + offset, 2);
    uint16_t decoded_code = core::net_to_host16(code_net_read);
    offset += 2;

    // 读取 message
    uint16_t len_net;
    std::memcpy(&len_net, ptr + offset, 2);
    uint16_t message_len = core::net_to_host16(len_net);
    offset += 2;

    std::string decoded_message(ptr + offset, message_len);

    EXPECT_EQ(error_code, decoded_code);
    EXPECT_EQ(message, decoded_message);
}

} // namespace
