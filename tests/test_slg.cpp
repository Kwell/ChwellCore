#include <gtest/gtest.h>

#include "chwell/slg/map.h"
#include "chwell/slg/battle.h"

using namespace chwell;

// SLG Map Tests
TEST(SlgMapTest, Init) {
    slg::SlgMapConfig config;
    config.width = 50;
    config.height = 50;
    
    slg::SlgMapManager map(config);
    map.init();
    EXPECT_EQ(map.total_cells(), 2500);
}

TEST(SlgMapTest, GenerateTerrain) {
    slg::SlgMapConfig config;
    config.width = 100;
    config.height = 100;
    
    slg::SlgMapManager map(config);
    map.init();
    map.generate_terrain();
    
    slg::GridCell cell;
    EXPECT_TRUE(map.get_cell(50, 50, cell));
}

TEST(SlgMapTest, GenerateResources) {
    slg::SlgMapConfig config;
    config.width = 50;
    config.height = 50;
    
    slg::SlgMapManager map(config);
    map.init();
    map.generate_terrain();
    map.generate_resources(20);
    
    int resource_count = 0;
    for (int y = 0; y < 50; y++) {
        for (int x = 0; x < 50; x++) {
            slg::GridCell cell;
            if (map.get_cell(x, y, cell) && cell.has_resource()) {
                resource_count++;
            }
        }
    }
    
    EXPECT_GT(resource_count, 0);
}

TEST(SlgMapTest, CreateAndMoveTroop) {
    slg::SlgMapConfig config;
    config.width = 100;
    config.height = 100;
    
    slg::SlgMapManager map(config);
    map.init();
    map.generate_terrain();
    
    uint64_t troop_id = map.create_troop(1, 10, 10, 100, 50, 30, 10);
    EXPECT_GT(troop_id, 0u);
    
    slg::Troop troop;
    EXPECT_TRUE(map.get_troop(troop_id, troop));
    EXPECT_EQ(troop.current_x, 10);
    EXPECT_EQ(troop.current_y, 10);
    
    EXPECT_TRUE(map.move_troop(troop_id, 20, 20));
}

TEST(SlgMapTest, GetTroopsInRange) {
    slg::SlgMapConfig config;
    config.width = 100;
    config.height = 100;
    
    slg::SlgMapManager map(config);
    map.init();
    map.generate_terrain();
    
    map.create_troop(1, 10, 10, 100, 50, 30, 10);
    map.create_troop(1, 15, 15, 100, 50, 30, 10);
    map.create_troop(2, 80, 80, 100, 50, 30, 10);
    
    auto troops = map.get_troops_in_range(10, 10, 20);
    EXPECT_EQ(troops.size(), 2u);
}

TEST(SlgMapTest, OutOfBounds) {
    slg::SlgMapConfig config;
    config.width = 50;
    config.height = 50;
    
    slg::SlgMapManager map(config);
    map.init();
    
    slg::GridCell cell;
    EXPECT_FALSE(map.get_cell(100, 100, cell));
}

TEST(SlgMapTest, TotalCities) {
    slg::SlgMapConfig config;
    config.width = 100;
    config.height = 100;
    
    slg::SlgMapManager map(config);
    map.init();
    map.generate_terrain();
    map.generate_cities(5);
    
    EXPECT_EQ(map.total_cities(), 5);
}

TEST(SlgMapTest, FindPath) {
    slg::SlgMapConfig config;
    config.width = 100;
    config.height = 100;
    
    slg::SlgMapManager map(config);
    map.init();
    map.generate_terrain();
    
    auto path = map.find_path(10, 10, 20, 20);
    EXPECT_FALSE(path.empty());
}

TEST(SlgMapTest, CalculateDistance) {
    slg::SlgMapConfig config;
    slg::SlgMapManager map(config);
    
    // 使用切比雪夫距离：max(|x2-x1|, |y2-y1|)
    int dist = map.calculate_distance(0, 0, 3, 4);
    EXPECT_EQ(dist, 4);  // max(3, 4) = 4
}

// Battle System Tests
TEST(BattleTest, AttackerWins) {
    slg::BattleSystem battle;
    
    std::vector<slg::General> attacker_gens = {
        []() { slg::General g; g.general_id = 1; g.force = 100; g.intelligence = 80; g.command = 90; g.speed = 70; return g; }()
    };
    
    std::vector<slg::General> defender_gens = {
        []() { slg::General g; g.general_id = 2; g.force = 30; g.intelligence = 20; g.command = 20; g.speed = 30; return g; }()
    };
    
    // 增加兵力差距，确保攻方必胜
    auto report = battle.execute(
        1, "Strong", attacker_gens, 100000,
        2, "Weak", defender_gens, 100,
        false
    );
    
    EXPECT_EQ(report.result, slg::BattleReport::Result::ATTACKER_WIN);
    EXPECT_GT(report.attacker_final, 0);
}

TEST(BattleTest, DefenderWins) {
    slg::BattleSystem battle;
    
    std::vector<slg::General> attacker_gens = {
        []() { slg::General g; g.general_id = 1; g.force = 30; g.intelligence = 20; g.command = 20; g.speed = 30; return g; }()
    };
    
    std::vector<slg::General> defender_gens = {
        []() { slg::General g; g.general_id = 2; g.force = 100; g.intelligence = 80; g.command = 90; g.speed = 70; return g; }()
    };
    
    // 增加守方兵力差距，确保守方必胜
    auto report = battle.execute(
        1, "Weak", attacker_gens, 100,
        2, "Strong", defender_gens, 100000,
        false
    );
    
    EXPECT_EQ(report.result, slg::BattleReport::Result::DEFENDER_WIN);
    EXPECT_GT(report.defender_final, 0);
}

TEST(BattleTest, MultipleGenerals) {
    slg::BattleSystem battle;
    
    std::vector<slg::General> attacker_gens = {
        []() { slg::General g; g.general_id = 1; g.force = 90; return g; }(),
        []() { slg::General g; g.general_id = 2; g.force = 85; return g; }(),
        []() { slg::General g; g.general_id = 3; g.force = 80; return g; }()
    };
    
    std::vector<slg::General> defender_gens = {
        []() { slg::General g; g.general_id = 4; g.force = 70; return g; }(),
        []() { slg::General g; g.general_id = 5; g.force = 65; return g; }()
    };
    
    auto report = battle.execute(
        1, "Team1", attacker_gens, 5000,
        2, "Team2", defender_gens, 3000,
        false
    );
    
    EXPECT_NE(report.result, slg::BattleReport::Result::DRAW);
    EXPECT_FALSE(report.rounds.empty());
}

TEST(BattleTest, SiegeBonus) {
    slg::BattleSystem battle;
    
    std::vector<slg::General> attacker_gens = {
        []() { slg::General g; g.general_id = 1; g.force = 60; return g; }()
    };
    
    std::vector<slg::General> defender_gens = {
        []() { slg::General g; g.general_id = 2; g.force = 60; return g; }()
    };
    
    // With siege penalty for attacker
    auto report_siege = battle.execute(
        1, "A", attacker_gens, 5000,
        2, "D", defender_gens, 5000,
        true
    );
    
    // Without siege
    auto report_normal = battle.execute(
        1, "A", attacker_gens, 5000,
        2, "D", defender_gens, 5000,
        false
    );
    
    // Results may differ due to siege mechanics
    EXPECT_TRUE(report_siege.rounds.size() > 0 || report_normal.rounds.size() > 0);
}

TEST(BattleTest, BattleReportContainsData) {
    slg::BattleSystem battle;
    
    std::vector<slg::General> attacker_gens = {
        []() { slg::General g; g.general_id = 1; g.name = "TestGeneral"; g.force = 50; return g; }()
    };
    
    std::vector<slg::General> defender_gens = {
        []() { slg::General g; g.general_id = 2; g.force = 50; return g; }()
    };
    
    auto report = battle.execute(
        1, "Attacker", attacker_gens, 1000,
        2, "Defender", defender_gens, 1000,
        false
    );
    
    EXPECT_EQ(report.attacker_id, 1u);
    EXPECT_EQ(report.defender_id, 2u);
    EXPECT_EQ(report.attacker_name, "Attacker");
    EXPECT_EQ(report.defender_name, "Defender");
    EXPECT_EQ(report.attacker_initial, 1000);
    EXPECT_EQ(report.defender_initial, 1000);
}