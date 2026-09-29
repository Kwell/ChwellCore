#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chwell {
namespace game {

// ========== 好友 / 社交 ==========
//
// 单向关注 + 双向好友：add_friend 需双方都添加才是好友（or 单向关注）。
// - follow / unfollow：单向
// - add_friend / remove_friend：双向
// - is_friend / is_following / friends_of / followers_of
// - 禁言/黑名单：block / unblock / is_blocked

class SocialGraph {
public:
    void follow(const std::string& from, const std::string& to) {
        if (from.empty() || to.empty() || from == to) return;
        std::lock_guard<std::mutex> lock(mu_);
        following_[from].insert(to);
        followers_[to].insert(from);
    }

    void unfollow(const std::string& from, const std::string& to) {
        std::lock_guard<std::mutex> lock(mu_);
        auto f = following_.find(from);
        if (f != following_.end()) f->second.erase(to);
        auto r = followers_.find(to);
        if (r != followers_.end()) r->second.erase(from);
    }

    void add_friend(const std::string& a, const std::string& b) {
        if (a.empty() || b.empty() || a == b) return;
        follow(a, b);
        follow(b, a);
    }

    void remove_friend(const std::string& a, const std::string& b) {
        unfollow(a, b);
        unfollow(b, a);
    }

    bool is_following(const std::string& from, const std::string& to) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = following_.find(from);
        return it != following_.end() && it->second.count(to) > 0;
    }

    bool is_friend(const std::string& a, const std::string& b) const {
        return is_following(a, b) && is_following(b, a);
    }

    std::vector<std::string> friends_of(const std::string& id) const {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = following_.find(id);
        if (it == following_.end()) return out;
        for (const auto& other : it->second) {
            auto fit = following_.find(other);
            if (fit != following_.end() && fit->second.count(id)) {
                out.push_back(other);
            }
        }
        return out;
    }

    std::vector<std::string> followers_of(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = followers_.find(id);
        if (it == followers_.end()) return {};
        return std::vector<std::string>(it->second.begin(), it->second.end());
    }

    void block(const std::string& from, const std::string& to) {
        if (from.empty() || to.empty()) return;
        std::lock_guard<std::mutex> lock(mu_);
        blocked_[from].insert(to);
    }

    void unblock(const std::string& from, const std::string& to) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = blocked_.find(from);
        if (it != blocked_.end()) it->second.erase(to);
    }

    bool is_blocked(const std::string& from, const std::string& to) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = blocked_.find(from);
        return it != blocked_.end() && it->second.count(to) > 0;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unordered_set<std::string>> following_;
    std::unordered_map<std::string, std::unordered_set<std::string>> followers_;
    std::unordered_map<std::string, std::unordered_set<std::string>> blocked_;
};

// ========== 匹配 ==========
//
// 按分段（bucket）排队，凑满 team_size 即成局。
// - enqueue(player_id, score)：进对应分段队列
// - try_match()：任一分段凑满 team_size 返回一组（队首优先，FIFO）
// - cancel(player_id)
// - queue_size(bucket)

class Matchmaker {
public:
    explicit Matchmaker(int team_size = 2, int score_bucket_width = 100)
        : team_size_(team_size < 2 ? 2 : team_size),
          bucket_width_(score_bucket_width < 1 ? 1 : score_bucket_width) {}

    bool enqueue(const std::string& player_id, std::int64_t score, std::int64_t now = 0) {
        if (player_id.empty()) return false;
        std::lock_guard<std::mutex> lock(mu_);
        if (queued_.count(player_id)) return false;
        int bucket = bucket_of(score);
        Waiter w;
        w.player_id = player_id;
        w.score = score;
        w.enqueue_time = now;
        queues_[bucket].push_back(w);
        queued_[player_id] = bucket;
        return true;
    }

    bool cancel(const std::string& player_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = queued_.find(player_id);
        if (it == queued_.end()) return false;
        auto& q = queues_[it->second];
        for (auto i = q.begin(); i != q.end(); ++i) {
            if (i->player_id == player_id) {
                q.erase(i);
                break;
            }
        }
        queued_.erase(it);
        return true;
    }

    // 任一分段凑满 team_size 即返回该组玩家 id；否则返回空
    std::vector<std::string> try_match() {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [bucket, q] : queues_) {
            if (static_cast<int>(q.size()) < team_size_) continue;
            std::vector<std::string> team;
            team.reserve(team_size_);
            for (int i = 0; i < team_size_; ++i) {
                team.push_back(q.front().player_id);
                queued_.erase(q.front().player_id);
                q.pop_front();
            }
            return team;
        }
        return {};
    }

    std::size_t queue_size(std::int64_t score) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = queues_.find(bucket_of(score));
        return it == queues_.end() ? 0 : it->second.size();
    }

    std::size_t total_waiting() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t n = 0;
        for (const auto& [b, q] : queues_) n += q.size();
        return n;
    }

private:
    struct Waiter {
        std::string player_id;
        std::int64_t score = 0;
        std::int64_t enqueue_time = 0;
    };

    int bucket_of(std::int64_t score) const {
        if (score < 0) score = 0;
        return static_cast<int>(score / bucket_width_);
    }

    int team_size_;
    int bucket_width_;
    mutable std::mutex mu_;
    std::unordered_map<int, std::deque<Waiter>> queues_;
    std::unordered_map<std::string, int> queued_;
};

}  // namespace game
}  // namespace chwell
