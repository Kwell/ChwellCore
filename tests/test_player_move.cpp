#include <gtest/gtest.h>
#include "chwell/game/player_move.h"
#include "chwell/core/endian.h"

using namespace chwell;
using namespace chwell::game;

namespace {

// 测试 float 编码/解码
TEST(PlayerMoveTest, EncodeDecodeFloat) {
    float original = 123.456f;

    // 编码（4字节）
    std::string encoded;
    encoded.resize(4);
    std::memcpy(encoded.data(), &original, 4);

    // 解码
    float decoded;
    std::memcpy(&decoded, encoded.data(), 4);

    EXPECT_FLOAT_EQ(original, decoded);
}

// 测试 PlayerPosition 结构
TEST(PlayerMoveTest, PlayerPositionBasic) {
    PlayerPosition pos(1.0f, 2.0f, 3.0f);

    EXPECT_FLOAT_EQ(1.0f, pos.x);
    EXPECT_FLOAT_EQ(2.0f, pos.y);
    EXPECT_FLOAT_EQ(3.0f, pos.z);
}

// 测试玩家位置更新和查询
TEST(PlayerMoveTest, PlayerPositionUpdateAndQuery) {
    PlayerMoveComponent move_comp;

    // 更新玩家位置
    PlayerPosition pos1(10.0f, 20.0f, 30.0f);
    move_comp.update_player_position("player123", pos1);

    // 查询玩家位置
    PlayerPosition pos2;
    bool found = move_comp.get_player_position("player123", pos2);

    EXPECT_TRUE(found);
    EXPECT_FLOAT_EQ(pos1.x, pos2.x);
    EXPECT_FLOAT_EQ(pos1.y, pos2.y);
    EXPECT_FLOAT_EQ(pos1.z, pos2.z);

    // 查询不存在的玩家
    PlayerPosition pos3;
    found = move_comp.get_player_position("not_found", pos3);

    EXPECT_FALSE(found);
}

// 测试多个玩家位置
TEST(PlayerMoveTest, MultiplePlayerPositions) {
    PlayerMoveComponent move_comp;

    // 更新多个玩家位置
    move_comp.update_player_position("player1", PlayerPosition(1.0f, 2.0f, 3.0f));
    move_comp.update_player_position("player2", PlayerPosition(10.0f, 20.0f, 30.0f));
    move_comp.update_player_position("player3", PlayerPosition(100.0f, 200.0f, 300.0f));

    // 查询所有玩家位置
    PlayerPosition pos1, pos2, pos3;
    bool found1 = move_comp.get_player_position("player1", pos1);
    bool found2 = move_comp.get_player_position("player2", pos2);
    bool found3 = move_comp.get_player_position("player3", pos3);

    EXPECT_TRUE(found1);
    EXPECT_TRUE(found2);
    EXPECT_TRUE(found3);

    EXPECT_FLOAT_EQ(1.0f, pos1.x);
    EXPECT_FLOAT_EQ(10.0f, pos2.x);
    EXPECT_FLOAT_EQ(100.0f, pos3.x);
}

// 测试玩家位置更新
TEST(PlayerMoveTest, PlayerPositionUpdate) {
    PlayerMoveComponent move_comp;

    // 初始位置
    PlayerPosition pos1(1.0f, 2.0f, 3.0f);
    move_comp.update_player_position("player123", pos1);

    // 查询初始位置
    PlayerPosition pos2;
    move_comp.get_player_position("player123", pos2);
    EXPECT_FLOAT_EQ(1.0f, pos2.x);
    EXPECT_FLOAT_EQ(2.0f, pos2.y);
    EXPECT_FLOAT_EQ(3.0f, pos2.z);

    // 更新位置
    PlayerPosition pos3(10.0f, 20.0f, 30.0f);
    move_comp.update_player_position("player123", pos3);

    // 查询更新后的位置
    PlayerPosition pos4;
    move_comp.get_player_position("player123", pos4);
    EXPECT_FLOAT_EQ(10.0f, pos4.x);
    EXPECT_FLOAT_EQ(20.0f, pos4.y);
    EXPECT_FLOAT_EQ(30.0f, pos4.z);
}

} // namespace
