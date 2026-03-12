#include "chwell/slg/battle.h"
#include <chrono>
#include <sstream>

namespace chwell {
namespace slg {

BattleSystem::BattleSystem(const BattleConfig& config)
    : config_(config) {
    // 使用时间作为随机种子
    auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    rng_.seed(static_cast<unsigned>(seed));
}

BattleReport BattleSystem::execute(
    uint64_t attacker_id, const std::string& attacker_name,
    const std::vector<General>& attacker_generals,
    int attacker_troops,
    
    uint64_t defender_id, const std::string& defender_name,
    const std::vector<General>& defender_generals,
    int defender_troops,
    
    bool is_siege) {
    
    BattleReport report;
    report.report_id = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    report.timestamp = report.report_id;
    
    report.attacker_id = attacker_id;
    report.attacker_name = attacker_name;
    report.attacker_generals = attacker_generals;
    report.attacker_initial = attacker_troops;
    
    report.defender_id = defender_id;
    report.defender_name = defender_name;
    report.defender_generals = defender_generals;
    report.defender_initial = defender_troops;
    
    int attacker_remaining = attacker_troops;
    int defender_remaining = defender_troops;
    
    // 战斗回合
    for (int round = 1; round <= config_.max_rounds; ++round) {
        BattleRound br;
        br.round = round;
        
        // 攻方行动
        for (const auto& general : attacker_generals) {
            if (defender_remaining <= 0) break;
            if (attacker_remaining <= 0) break;
            
            // 选择目标
            int target_idx = rng_() % std::max(1, (int)defender_generals.size());
            const auto& target = defender_generals[target_idx];
            
            bool use_skill = config_.use_skills && (rng_() % 100 < 30);
            int damage = calculate_damage(general, target, use_skill);
            
            // 应用伤害
            int actual_damage = std::min(damage, defender_remaining);
            defender_remaining -= actual_damage;
            
            BattleRound::Action action;
            action.attacker_id = general.general_id;
            action.target_id = target.general_id;
            action.damage = actual_damage;
            action.is_skill = use_skill;
            action.skill_id = use_skill ? general.skill_id : 0;
            action.desc = generate_action_desc(general, target, actual_damage, use_skill);
            
            br.attacker_actions.push_back(action);
        }
        
        // 守方行动
        if (defender_remaining > 0) {
            for (const auto& general : defender_generals) {
                if (attacker_remaining <= 0) break;
                if (defender_remaining <= 0) break;
                
                int target_idx = rng_() % std::max(1, (int)attacker_generals.size());
                const auto& target = attacker_generals[target_idx];
                
                bool use_skill = config_.use_skills && (rng_() % 100 < 30);
                int damage = calculate_damage(general, target, use_skill);
                
                int actual_damage = std::min(damage, attacker_remaining);
                attacker_remaining -= actual_damage;
                
                BattleRound::Action action;
                action.attacker_id = general.general_id;
                action.target_id = target.general_id;
                action.damage = actual_damage;
                action.is_skill = use_skill;
                action.skill_id = use_skill ? general.skill_id : 0;
                action.desc = generate_action_desc(general, target, actual_damage, use_skill);
                
                br.defender_actions.push_back(action);
            }
        }
        
        br.attacker_remaining = attacker_remaining;
        br.defender_remaining = defender_remaining;
        
        report.rounds.push_back(br);
        
        // 检查战斗结束
        if (attacker_remaining <= 0 || defender_remaining <= 0) {
            break;
        }
    }
    
    // 确定结果
    report.attacker_final = attacker_remaining;
    report.defender_final = defender_remaining;
    
    if (attacker_remaining > defender_remaining) {
        report.result = BattleReport::Result::ATTACKER_WIN;
    } else if (defender_remaining > attacker_remaining) {
        report.result = BattleReport::Result::DEFENDER_WIN;
    } else {
        report.result = BattleReport::Result::DRAW;
    }
    
    // 计算损失
    int attacker_loss = attacker_troops - attacker_remaining;
    int defender_loss = defender_troops - defender_remaining;
    
    // 简化的资源损失计算
    report.food_loss = attacker_loss * 10;
    report.wood_loss = attacker_loss * 5;
    report.stone_loss = attacker_loss * 3;
    report.iron_loss = attacker_loss * 2;
    
    // 回调
    if (report_callback_) {
        report_callback_(report);
    }
    
    CHWELL_LOG_INFO("Battle completed: attacker=" << attacker_id 
                  << " vs defender=" << defender_id
                  << ", result=" << (int)report.result);
    
    return report;
}

int BattleSystem::calculate_power(const std::vector<General>& generals, int troops) const {
    int total_general_power = 0;
    for (const auto& general : generals) {
        total_general_power += calculate_general_power(general);
    }
    
    // 战斗力 = 武将属性 + 兵力 * 10
    return total_general_power + troops * 10;
}

int BattleSystem::calculate_general_power(const General& general) const {
    // 武将战斗力 = 武力 + 智力 + 统率 + 速度/2
    return general.force + general.intelligence + general.command + general.speed / 2;
}

int BattleSystem::calculate_damage(const General& attacker, const General& defender, bool use_skill) {
    int base = config_.base_damage;
    
    // 武力影响基础伤害
    int force_damage = static_cast<int>((attacker.force - defender.command / 2) * config_.force_ratio);
    
    // 智力影响技能伤害
    int intel_damage = 0;
    if (use_skill) {
        intel_damage = calculate_skill_damage(attacker, attacker.skill_id, attacker.skill_level);
    }
    
    // 随机波动 (0.8 - 1.2)
    double random_factor = 0.8 + (rng_() % 41) / 100.0;
    
    int total_damage = static_cast<int>((base + force_damage + intel_damage) * random_factor);
    
    // 闪避检查
    if (BattleFormula::check_dodge(defender.speed, attacker.force)) {
        total_damage = static_cast<int>(total_damage * 0.3);  // 闪避成功，伤害减少70%
    }
    
    // 暴击检查
    if (BattleFormula::check_critical(attacker.force, defender.speed)) {
        total_damage = static_cast<int>(total_damage * BattleFormula::critical_multiplier());
    }
    
    return std::max(1, total_damage);  // 最少1点伤害
}

int BattleSystem::calculate_skill_damage(const General& attacker, int skill_id, int skill_level) {
    // 简化：技能伤害 = 智力 * 技能等级 * 系数
    int base_skill_power = 50;  // 基础技能威力
    return static_cast<int>(attacker.intelligence * skill_level * config_.intel_ratio * base_skill_power / 100);
}

std::string BattleSystem::generate_action_desc(const General& attacker, const General& defender,
                                                int damage, bool is_skill) {
    std::ostringstream oss;
    oss << attacker.name;
    
    if (is_skill) {
        oss << " 发动技能，对 " << defender.name << " 造成 " << damage << " 点伤害！";
    } else {
        oss << " 攻击 " << defender.name << "，造成 " << damage << " 点伤害。";
    }
    
    return oss.str();
}

//=============================================================================
// BattleFormula 实现
//=============================================================================

int BattleFormula::base_damage(int attacker_force, int defender_command, int base) {
    // 伤害 = 基础 + (武力 - 防御/2) * 系数
    int diff = attacker_force - defender_command / 2;
    return base + std::max(0, diff * 2);
}

int BattleFormula::skill_damage(int attacker_intel, int skill_power, int skill_level) {
    // 技能伤害 = 智力 * 技能威力 * 技能等级 / 100
    return attacker_intel * skill_power * skill_level / 100;
}

bool BattleFormula::check_critical(int attacker_force, int defender_speed) {
    // 暴击率 = 武力 / (武力 + 敌方速度) * 0.3
    // 简化：武力比速度高越多，暴击率越高
    int total = attacker_force + defender_speed;
    if (total <= 0) return false;
    
    int chance = attacker_force * 30 / total;
    return (rand() % 100) < chance;
}

int BattleFormula::critical_multiplier() {
    return 150;  // 1.5倍伤害
}

bool BattleFormula::check_dodge(int defender_speed, int attacker_force) {
    // 闪避率 = 速度 / (速度 + 敌方武力) * 0.2
    int total = defender_speed + attacker_force;
    if (total <= 0) return false;
    
    int chance = defender_speed * 20 / total;
    return (rand() % 100) < chance;
}

double BattleFormula::damage_reduction(int defender_command) {
    // 伤害减免 = 统率 / (统率 + 200)
    return static_cast<double>(defender_command) / (defender_command + 200.0);
}

} // namespace slg
} // namespace chwell