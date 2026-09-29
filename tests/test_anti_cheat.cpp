#include <gtest/gtest.h>

#include <string>

#include "chwell/game/anti_cheat.h"

using namespace chwell::game;

TEST(AntiCheatTest, FirstMoveAlwaysOk) {
    AntiCheat ac;
    auto v = ac.check_move("p1", 0, 0, 1000);
    EXPECT_TRUE(v.ok);
}

TEST(AntiCheatTest, NormalSpeedAccepted) {
    AntiCheat ac({/*max_speed=*/100.0, /*max_jump=*/500.0});
    EXPECT_TRUE(ac.check_move("p1", 0, 0, 1000).ok);
    // 50 单位 / 1 秒 = 50 速度 < 100
    EXPECT_TRUE(ac.check_move("p1", 50, 0, 2000).ok);
}

TEST(AntiCheatTest, SpeedHackDetected) {
    AntiCheat ac({/*max_speed=*/100.0, /*max_jump=*/500.0});
    ASSERT_TRUE(ac.check_move("p1", 0, 0, 1000).ok);
    // 90 单位 / 0.1 秒 = 900 速度
    auto v = ac.check_move("p1", 90, 0, 1100);
    EXPECT_FALSE(v.ok);
    EXPECT_EQ("speed_hack", v.reason);
}

TEST(AntiCheatTest, TeleportDetected) {
    AntiCheat ac({/*max_speed=*/100.0, /*max_jump=*/500.0});
    ASSERT_TRUE(ac.check_move("p1", 0, 0, 1000).ok);
    auto v = ac.check_move("p1", 10000, 0, 1001);
    EXPECT_FALSE(v.ok);
    EXPECT_EQ("teleport", v.reason);
}

TEST(AntiCheatTest, SpeedZeroMeansUnlimited) {
    AntiCheat ac({/*max_speed=*/0.0, /*max_jump=*/0.0});
    ASSERT_TRUE(ac.check_move("p1", 0, 0, 1000).ok);
    EXPECT_TRUE(ac.check_move("p1", 99999, 0, 1001).ok);
}

TEST(AntiCheatTest, ActionFloodDetected) {
    AntiCheat ac;
    AntiCheat::RateRule rr;
    rr.max_count = 3;
    rr.window_ms = 1000;
    AntiCheat ac2(AntiCheat::SpeedRule(), rr);

    EXPECT_TRUE(ac2.check_action("p1", 1000).ok);
    EXPECT_TRUE(ac2.check_action("p1", 1001).ok);
    EXPECT_TRUE(ac2.check_action("p1", 1002).ok);
    auto v = ac2.check_action("p1", 1003);
    EXPECT_FALSE(v.ok);
    EXPECT_EQ("action_flood", v.reason);
}

TEST(AntiCheatTest, ActionWindowSlides) {
    AntiCheat::RateRule rr;
    rr.max_count = 2;
    rr.window_ms = 1000;
    AntiCheat ac(AntiCheat::SpeedRule(), rr);

    EXPECT_TRUE(ac.check_action("p1", 1000).ok);
    EXPECT_TRUE(ac.check_action("p1", 1001).ok);
    EXPECT_FALSE(ac.check_action("p1", 1002).ok);
    // 越过窗口后旧记录淘汰，可继续
    EXPECT_TRUE(ac.check_action("p1", 2100).ok);
}

TEST(AntiCheatTest, ResetClearsState) {
    AntiCheat ac({100.0, 500.0});
    ASSERT_TRUE(ac.check_move("p1", 0, 0, 1000).ok);
    ac.reset("p1");
    // 重置后视为首点
    EXPECT_TRUE(ac.check_move("p1", 10000, 0, 1001).ok);
}

TEST(AntiCheatTest, PlayersAreIndependent) {
    AntiCheat ac({100.0, 500.0});
    ASSERT_TRUE(ac.check_move("a", 0, 0, 1000).ok);
    ASSERT_TRUE(ac.check_move("b", 0, 0, 1000).ok);
    EXPECT_FALSE(ac.check_move("a", 200, 0, 1010).ok);  // a 超速
    EXPECT_TRUE(ac.check_move("b", 5, 0, 1100).ok);     // b 正常
}
