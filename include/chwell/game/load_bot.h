#pragma once

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace chwell {
namespace game {

// ========== 压测机器人 ==========
//
// LoadBot：生成模拟玩家的行为脚本（登录 → 心跳 → 移动 / 聊天），
// 供压测工具按时间轴回放。纯逻辑，不触网；真正的 TCP 发送由上层执行器完成。
//
// - 生成 N 个 bot 的 action 时间表
// - action 类型：Login / Heartbeat / Move / Chat / Logout
// - 支持固定间隔或抖动（jitter_ms）

struct BotAction {
    std::int64_t time_ms = 0;
    std::string bot_id;
    std::uint16_t cmd = 0;
    double x = 0;
    double y = 0;
    std::string text;
};

struct LoadBotConfig {
    int bot_count = 10;
    std::int64_t duration_ms = 10000;
    std::int64_t heartbeat_interval_ms = 3000;
    std::int64_t move_interval_ms = 200;
    std::int64_t chat_interval_ms = 5000;
    std::int64_t jitter_ms = 50;
    std::uint32_t seed = 1;
    double map_width = 1000;
    double map_height = 1000;
};

class LoadBotPlanner {
public:
    using Config = LoadBotConfig;

    enum ActionType : std::uint16_t {
        LOGIN = 1,
        HEARTBEAT = 2,
        MOVE = 3,
        CHAT = 4,
        LOGOUT = 5,
    };

    explicit LoadBotPlanner(Config cfg = Config()) : cfg_(cfg) {}

    // 生成完整时间表（按 time_ms 升序）
    std::vector<BotAction> plan() const {
        std::mt19937 rng(cfg_.seed);
        std::vector<BotAction> actions;

        for (int i = 0; i < cfg_.bot_count; ++i) {
            std::string bot = "bot_" + std::to_string(i);
            std::int64_t t = jitter(rng);

            // Login
            actions.push_back(make(t, bot, LOGIN, 0, 0, ""));

            std::int64_t next_hb = t + cfg_.heartbeat_interval_ms + jitter(rng);
            std::int64_t next_move = t + cfg_.move_interval_ms + jitter(rng);
            std::int64_t next_chat = t + cfg_.chat_interval_ms + jitter(rng);

            double x = uniform(rng, 0, cfg_.map_width);
            double y = uniform(rng, 0, cfg_.map_height);

            while (t < cfg_.duration_ms) {
                // 选最近的下一动作
                std::int64_t next = next_hb;
                int kind = HEARTBEAT;
                if (next_move < next) { next = next_move; kind = MOVE; }
                if (next_chat < next) { next = next_chat; kind = CHAT; }

                if (next > cfg_.duration_ms) break;
                t = next;

                if (kind == HEARTBEAT) {
                    actions.push_back(make(t, bot, HEARTBEAT, 0, 0, ""));
                    next_hb = t + cfg_.heartbeat_interval_ms + jitter(rng);
                } else if (kind == MOVE) {
                    x = clamp(x + uniform(rng, -10, 10), 0, cfg_.map_width);
                    y = clamp(y + uniform(rng, -10, 10), 0, cfg_.map_height);
                    actions.push_back(make(t, bot, MOVE, x, y, ""));
                    next_move = t + cfg_.move_interval_ms + jitter(rng);
                } else {
                    actions.push_back(make(t, bot, CHAT, 0, 0, "hello from " + bot));
                    next_chat = t + cfg_.chat_interval_ms + jitter(rng);
                }
            }

            // Logout 收尾
            actions.push_back(make(cfg_.duration_ms, bot, LOGOUT, 0, 0, ""));
        }

        std::sort(actions.begin(), actions.end(),
                  [](const BotAction& a, const BotAction& b) {
                      if (a.time_ms != b.time_ms) return a.time_ms < b.time_ms;
                      return a.bot_id < b.bot_id;
                  });
        return actions;
    }

    const Config& config() const { return cfg_; }

private:
    BotAction make(std::int64_t t, const std::string& bot, std::uint16_t cmd,
                   double x, double y, const std::string& text) const {
        BotAction a;
        a.time_ms = t;
        a.bot_id = bot;
        a.cmd = cmd;
        a.x = x;
        a.y = y;
        a.text = text;
        return a;
    }

    std::int64_t jitter(std::mt19937& rng) const {
        if (cfg_.jitter_ms <= 0) return 0;
        std::uniform_int_distribution<std::int64_t> d(0, cfg_.jitter_ms);
        return d(rng);
    }

    double uniform(std::mt19937& rng, double lo, double hi) const {
        std::uniform_real_distribution<double> d(lo, hi);
        return d(rng);
    }

    static double clamp(double v, double lo, double hi) {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    Config cfg_;
};

}  // namespace game
}  // namespace chwell
