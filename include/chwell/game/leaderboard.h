#pragma once

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace chwell {
namespace game {

// ========== 排行榜 ==========
//
// 内存排行榜：分数越大排名越前（rank 从 1 开始）。
// - update：设置玩家分数（相同分数按更新时间后到者靠前，同分时 player_id 字典序稳定）
// - remove / clear
// - top(n)：前 N 名
// - rank_of / score_of：单人查询
// - count
//
// 适用单服内存榜；跨服聚合请在上层合并多个节点的 top(n)。

struct LeaderboardEntry {
    std::string player_id;
    std::int64_t score = 0;
    std::int64_t update_time = 0;
    int rank = 0;  // top()/rank_of 填充
};

class Leaderboard {
public:
    // 更新分数；score 相同时 update_time 大的排前面
    void update(const std::string& player_id, std::int64_t score, std::int64_t now = 0) {
        if (player_id.empty()) return;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = index_.find(player_id);
        if (it != index_.end()) {
            board_.erase(it->second);
            index_.erase(it);
        }
        Item item;
        item.player_id = player_id;
        item.score = score;
        item.update_time = now;
        auto hint = board_.insert(item).first;
        index_[player_id] = hint;
    }

    bool remove(const std::string& player_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = index_.find(player_id);
        if (it == index_.end()) return false;
        board_.erase(it->second);
        index_.erase(it);
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        board_.clear();
        index_.clear();
    }

    // 前 N 名（rank 从 1 起填入 entry.rank）
    std::vector<LeaderboardEntry> top(std::size_t n) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<LeaderboardEntry> out;
        out.reserve(n < board_.size() ? n : board_.size());
        int rank = 1;
        for (const auto& item : board_) {
            if (out.size() >= n) break;
            LeaderboardEntry e;
            e.player_id = item.player_id;
            e.score = item.score;
            e.update_time = item.update_time;
            e.rank = rank++;
            out.push_back(e);
        }
        return out;
    }

    // 0 表示不在榜上
    int rank_of(const std::string& player_id) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = index_.find(player_id);
        if (it == index_.end()) return 0;
        int rank = 1;
        for (auto i = board_.begin(); i != board_.end(); ++i, ++rank) {
            if (i->player_id == player_id) return rank;
        }
        return 0;
    }

    bool score_of(const std::string& player_id, std::int64_t& score) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = index_.find(player_id);
        if (it == index_.end()) return false;
        score = it->second->score;
        return true;
    }

    std::size_t count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return board_.size();
    }

private:
    struct Item {
        std::string player_id;
        std::int64_t score = 0;
        std::int64_t update_time = 0;
        // 分数高在前；同分 update_time 新在前；再同分按 id 字典序保证严格弱序
        bool operator<(const Item& o) const {
            if (score != o.score) return score > o.score;
            if (update_time != o.update_time) return update_time > o.update_time;
            return player_id < o.player_id;
        }
    };

    mutable std::mutex mu_;
    std::set<Item> board_;  // player_id 唯一，operator< 已定义严格弱序
    std::unordered_map<std::string, std::set<Item>::iterator> index_;
};

}  // namespace game
}  // namespace chwell
