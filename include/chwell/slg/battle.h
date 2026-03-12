#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <random>

#include "chwell/core/logger.h"

namespace chwell {
namespace slg {

// 前向声明
struct Troop;

// 武将数据
struct General {
    uint64_t general_id;
    std::string name;
    int rarity;             // 1-5星
    int level;
    
    // 属性
    int force;              // 武力
    int intelligence;       // 智力
    int command;            // 统率
    int charm;              // 魅力
    int speed;              // 速度
    
    // 技能
    int skill_id;
    int skill_level;
    
    General()
        : general_id(0), rarity(1), level(1)
        , force(50), intelligence(50), command(50), charm(50), speed(50)
        , skill_id(0), skill_level(1) {}
};

// 战报回合
struct BattleRound {
    int round;
    
    // 攻方行动
    struct Action {
        uint64_t attacker_id;
        uint64_t target_id;
        int damage;
        int skill_id;
        bool is_skill;
        std::string desc;
    };
    
    std::vector<Action> attacker_actions;
    std::vector<Action> defender_actions;
    
    // 回合结束时的状态
    int attacker_remaining;
    int defender_remaining;
};

// 战报
struct BattleReport {
    uint64_t report_id;
    int64_t timestamp;
    
    // 攻方信息
    uint64_t attacker_id;
    std::string attacker_name;
    std::vector<General> attacker_generals;
    int attacker_initial;
    int attacker_final;
    
    // 守方信息
    uint64_t defender_id;
    std::string defender_name;
    std::vector<General> defender_generals;
    int defender_initial;
    int defender_final;
    
    // 结果
    enum class Result {
        ATTACKER_WIN,
        DEFENDER_WIN,
        DRAW
    } result;
    
    std::vector<BattleRound> rounds;
    
    // 奖励/损失
    int64_t food_loss;
    int64_t wood_loss;
    int64_t stone_loss;
    int64_t iron_loss;
    
    BattleReport()
        : report_id(0), timestamp(0)
        , attacker_id(0), defender_id(0)
        , attacker_initial(0), attacker_final(0)
        , defender_initial(0), defender_final(0)
        , result(Result::DRAW)
        , food_loss(0), wood_loss(0), stone_loss(0), iron_loss(0) {}
    
    bool attacker_won() const { return result == Result::ATTACKER_WIN; }
};

// 战斗配置
struct BattleConfig {
    int max_rounds;         // 最大回合数
    bool use_skills;        // 是否使用技能
    int base_damage;        // 基础伤害
    double force_ratio;     // 武力伤害系数
    double intel_ratio;     // 智力伤害系数
    
    BattleConfig()
        : max_rounds(8), use_skills(true)
        , base_damage(100), force_ratio(2.0), intel_ratio(1.5) {}
};

// 战斗系统
class BattleSystem {
public:
    using ReportCallback = std::function<void(const BattleReport&)>;
    
    explicit BattleSystem(const BattleConfig& config = BattleConfig());
    
    // 执行战斗
    BattleReport execute(
        uint64_t attacker_id, const std::string& attacker_name,
        const std::vector<General>& attacker_generals,
        int attacker_troops,
        
        uint64_t defender_id, const std::string& defender_name,
        const std::vector<General>& defender_generals,
        int defender_troops,
        
        bool is_siege = false  // 是否攻城
    );
    
    // 计算部队战斗力
    int calculate_power(const std::vector<General>& generals, int troops) const;
    
    // 计算单个武将属性
    int calculate_general_power(const General& general) const;
    
    // 设置回调
    void set_report_callback(ReportCallback cb) { report_callback_ = std::move(cb); }
    
    const BattleConfig& config() const { return config_; }
    
private:
    // 计算伤害
    int calculate_damage(const General& attacker, const General& defender, bool use_skill);
    
    // 计算技能伤害
    int calculate_skill_damage(const General& attacker, int skill_id, int skill_level);
    
    // 生成战斗描述
    std::string generate_action_desc(const General& attacker, const General& defender,
                                     int damage, bool is_skill);
    
    BattleConfig config_;
    ReportCallback report_callback_;
    
    mutable std::mt19937 rng_;
};

// 简化的战斗公式（可配置）
class BattleFormula {
public:
    // 计算基础伤害
    static int base_damage(int attacker_force, int defender_command, int base);
    
    // 计算技能伤害
    static int skill_damage(int attacker_intel, int skill_power, int skill_level);
    
    // 计算暴击
    static bool check_critical(int attacker_force, int defender_speed);
    static int critical_multiplier();
    
    // 计算闪避
    static bool check_dodge(int defender_speed, int attacker_force);
    
    // 计算伤害减免
    static double damage_reduction(int defender_command);
};

} // namespace slg
} // namespace chwell