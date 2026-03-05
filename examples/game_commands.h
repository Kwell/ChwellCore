#pragma once

#include <cstdint>

// 与 protocol_server 兼容
namespace GameCmd {
    const std::uint16_t HEARTBEAT   = 3;
    const std::uint16_t LOGIN       = 10;
    const std::uint16_t LOGOUT      = 11;
    // 游戏大厅与房间
    const std::uint16_t LIST_ROOMS  = 20;
    const std::uint16_t CREATE_ROOM = 21;
    const std::uint16_t JOIN_ROOM   = 22;
    const std::uint16_t LEAVE_ROOM  = 23;
    const std::uint16_t ROOM_INFO   = 24;  // 服务端推送
    const std::uint16_t START_GAME  = 25;
    const std::uint16_t GAME_STATE  = 26;  // 服务端推送
    const std::uint16_t GAME_ACTION = 27;
}
