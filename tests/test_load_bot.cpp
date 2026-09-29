#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "chwell/game/load_bot.h"

using namespace chwell::game;

TEST(LoadBotPlannerTest, GeneratesLoginAndLogout) {
    LoadBotPlanner::Config cfg;
    cfg.bot_count = 2;
    cfg.duration_ms = 1000;
    cfg.jitter_ms = 0;
    LoadBotPlanner planner(cfg);

    auto actions = planner.plan();
    ASSERT_FALSE(actions.empty());

    int logins = 0, logouts = 0;
    for (const auto& a : actions) {
        if (a.cmd == LoadBotPlanner::LOGIN) ++logins;
        if (a.cmd == LoadBotPlanner::LOGOUT) ++logouts;
    }
    EXPECT_EQ(2, logins);
    EXPECT_EQ(2, logouts);
}

TEST(LoadBotPlannerTest, TimeSorted) {
    LoadBotPlanner::Config cfg;
    cfg.bot_count = 5;
    cfg.duration_ms = 3000;
    LoadBotPlanner planner(cfg);

    auto actions = planner.plan();
    for (std::size_t i = 1; i < actions.size(); ++i) {
        EXPECT_LE(actions[i - 1].time_ms, actions[i].time_ms);
    }
}

TEST(LoadBotPlannerTest, HeartbeatAndMovePresent) {
    LoadBotPlanner::Config cfg;
    cfg.bot_count = 1;
    cfg.duration_ms = 5000;
    cfg.heartbeat_interval_ms = 1000;
    cfg.move_interval_ms = 500;
    cfg.jitter_ms = 0;
    LoadBotPlanner planner(cfg);

    auto actions = planner.plan();
    int hb = 0, mv = 0;
    for (const auto& a : actions) {
        if (a.cmd == LoadBotPlanner::HEARTBEAT) ++hb;
        if (a.cmd == LoadBotPlanner::MOVE) ++mv;
    }
    EXPECT_GE(hb, 3);
    EXPECT_GE(mv, 5);
}

TEST(LoadBotPlannerTest, DeterministicWithSameSeed) {
    LoadBotPlanner::Config cfg;
    cfg.bot_count = 3;
    cfg.duration_ms = 2000;
    cfg.seed = 42;
    LoadBotPlanner p1(cfg);
    LoadBotPlanner p2(cfg);

    auto a1 = p1.plan();
    auto a2 = p2.plan();
    ASSERT_EQ(a1.size(), a2.size());
    for (std::size_t i = 0; i < a1.size(); ++i) {
        EXPECT_EQ(a1[i].time_ms, a2[i].time_ms);
        EXPECT_EQ(a1[i].bot_id, a2[i].bot_id);
        EXPECT_EQ(a1[i].cmd, a2[i].cmd);
    }
}

TEST(LoadBotPlannerTest, DifferentSeedDiffers) {
    LoadBotPlanner::Config c1;
    c1.bot_count = 3;
    c1.duration_ms = 2000;
    c1.seed = 1;
    LoadBotPlanner::Config c2 = c1;
    c2.seed = 2;
    LoadBotPlanner p1(c1);
    LoadBotPlanner p2(c2);

    auto a1 = p1.plan();
    auto a2 = p2.plan();
    ASSERT_EQ(a1.size(), a2.size());
    bool differ = false;
    for (std::size_t i = 0; i < a1.size(); ++i) {
        if (a1[i].time_ms != a2[i].time_ms || a1[i].x != a2[i].x) {
            differ = true;
            break;
        }
    }
    EXPECT_TRUE(differ);
}

TEST(LoadBotPlannerTest, MoveStaysInMap) {
    LoadBotPlanner::Config cfg;
    cfg.bot_count = 1;
    cfg.duration_ms = 3000;
    cfg.map_width = 100;
    cfg.map_height = 100;
    cfg.jitter_ms = 0;
    LoadBotPlanner planner(cfg);

    for (const auto& a : planner.plan()) {
        if (a.cmd == LoadBotPlanner::MOVE) {
            EXPECT_GE(a.x, 0);
            EXPECT_LE(a.x, 100);
            EXPECT_GE(a.y, 0);
            EXPECT_LE(a.y, 100);
        }
    }
}
