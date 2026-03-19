// 示例：游戏组件使用 Protobuf 编解码
// 展示如何使用 Protobuf 代替手动的编码/解码

#include <iostream>
#include <string>

// 假设已经生成了 game.pb.h
// #include "game.pb.h"
#include "chwell/core/logger.h"

// 这里使用简化的 Protobuf 消息定义
// 实际使用时应该使用 protoc 生成的代码

namespace {
    // 简化的 C2S_Login 消息
    struct C2S_Login {
        std::string player_id;
        std::string token;
    };

    // 简化的 S2C_Login 消息
    struct S2C_Login {
        bool ok;
        std::string message;
    };

    // 简化的 C2S_Chat 消息
    struct C2S_Chat {
        std::string room_id;
        std::string content;
    };

    // 简化的 S2C_Chat 消息
    struct S2C_Chat {
        std::string from_player_id;
        std::string content;
    };
}

// 示例：如何使用 Protobuf 编解码
int main() {
    CHWELL_LOG_INFO("Protobuf Game Components Demo");

    // 示例1：编码登录请求
    {
        C2S_Login login_req;
        login_req.player_id = "player123";
        login_req.token = "token456";

        // 使用 Protobuf 编码
        // std::string encoded = login_req.SerializeAsString();

        // 手动编码（模拟 Protobuf）
        std::string encoded = login_req.player_id + "|" + login_req.token;

        CHWELL_LOG_INFO("Encoded login request: " + encoded);

        // 使用 Protobuf 解码
        // C2S_Login decoded;
        // decoded.ParseFromString(encoded);

        // 手动解码（模拟 Protobuf）
        size_t sep = encoded.find('|');
        C2S_Login decoded;
        decoded.player_id = encoded.substr(0, sep);
        decoded.token = encoded.substr(sep + 1);

        CHWELL_LOG_INFO("Decoded player_id: " + decoded.player_id);
        CHWELL_LOG_INFO("Decoded token: " + decoded.token);
    }

    // 示例2：编码登录响应
    {
        S2C_Login login_resp;
        login_resp.ok = true;
        login_resp.message = "Login success";

        // 使用 Protobuf 编码
        // std::string encoded = login_resp.SerializeAsString();

        // 手动编码（模拟 Protobuf）
        std::string encoded = login_resp.ok ? "1|" : "0|";
        encoded += login_resp.message;

        CHWELL_LOG_INFO("Encoded login response: " + encoded);

        // 使用 Protobuf 解码
        // S2C_Login decoded;
        // decoded.ParseFromString(encoded);

        // 手动解码（模拟 Protobuf）
        size_t sep = encoded.find('|');
        S2C_Login decoded;
        decoded.ok = (encoded[0] == '1');
        decoded.message = encoded.substr(sep + 1);

        CHWELL_LOG_INFO("Decoded ok: " + std::to_string(decoded.ok));
        CHWELL_LOG_INFO("Decoded message: " + decoded.message);
    }

    CHWELL_LOG_INFO("Protobuf Game Components Demo completed");

    return 0;
}
