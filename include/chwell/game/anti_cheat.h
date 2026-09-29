#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace chwell {
namespace game {

// ========== 反作弊（基础校验） ==========
//
// 覆盖两类常见问题：
// 1. 速度/位移异常：两次位置采样按距离 / 时间差 估算速度，超阈值判违规
// 2. 操作频率：滑动窗口内计数，超限判违规
//
// 只做本地检测，不自动封禁；输出 violation 交由上层记录/踢下线。
// 时间单位统一毫秒。

struct CheatVerdict {
    bool ok = true;
    std::string reason;
};

struct AntiCheatSpeedRule {
    double max_speed = 100.0;      // 单位/秒，0 表示不限制
    double max_jump_dist = 500.0;  // 单次瞬移阈值，0 不限制
};

struct AntiCheatRateRule {
    int max_count = 30;            // 窗口内最大次数
    std::int64_t window_ms = 1000;
};

class AntiCheat {
public:
    using SpeedRule = AntiCheatSpeedRule;
    using RateRule = AntiCheatRateRule;

    explicit AntiCheat(SpeedRule speed = SpeedRule(), RateRule rate = RateRule())
        : speed_(speed), rate_(rate) {}

    // 校验一次移动；首次调用只记录位置
    CheatVerdict check_move(const std::string& player_id,
                            double x, double y,
                            std::int64_t now_ms) {
        std::lock_guard<std::mutex> lock(mu_);
        CheatVerdict v;
        auto it = last_pos_.find(player_id);
        if (it == last_pos_.end()) {
            last_pos_[player_id] = {x, y, now_ms};
            return v;
        }
        Position prev = it->second;
        last_pos_[player_id] = {x, y, now_ms};

        double dx = x - prev.x;
        double dy = y - prev.y;
        double dist = std::sqrt(dx * dx + dy * dy);

        if (speed_.max_jump_dist > 0 && dist > speed_.max_jump_dist) {
            v.ok = false;
            v.reason = "teleport";
            return v;
        }

        std::int64_t dt_ms = now_ms - prev.t;
        if (dt_ms <= 0) dt_ms = 1;
        if (speed_.max_speed > 0) {
            double speed = dist * 1000.0 / static_cast<double>(dt_ms);
            if (speed > speed_.max_speed) {
                v.ok = false;
                v.reason = "speed_hack";
            }
        }
        return v;
    }

    // 校验一次操作（任意命令字）
    CheatVerdict check_action(const std::string& player_id, std::int64_t now_ms) {
        std::lock_guard<std::mutex> lock(mu_);
        CheatVerdict v;
        auto& q = action_log_[player_id];
        while (!q.empty() && now_ms - q.front() > rate_.window_ms) {
            q.pop_front();
        }
        if (static_cast<int>(q.size()) >= rate_.max_count) {
            v.ok = false;
            v.reason = "action_flood";
            return v;
        }
        q.push_back(now_ms);
        return v;
    }

    void reset(const std::string& player_id) {
        std::lock_guard<std::mutex> lock(mu_);
        last_pos_.erase(player_id);
        action_log_.erase(player_id);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        last_pos_.clear();
        action_log_.clear();
    }

private:
    struct Position {
        double x = 0;
        double y = 0;
        std::int64_t t = 0;
    };

    SpeedRule speed_;
    RateRule rate_;
    std::mutex mu_;
    std::unordered_map<std::string, Position> last_pos_;
    std::unordered_map<std::string, std::deque<std::int64_t>> action_log_;
};

}  // namespace game
}  // namespace chwell
