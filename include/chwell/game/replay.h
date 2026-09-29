#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace chwell {
namespace game {

// ========== 回放 / 观战 ==========
//
// ReplayRecorder：按时间戳记录帧事件（输入 / 状态快照），支持导出回放流。
// SpectatorFeed：观战订阅，向订阅者推送最新帧（只读旁路）。
//
// 回放格式为内存结构，序列化由上层决定；本模块保证帧序与时间戳单调。

struct ReplayEvent {
    std::int64_t time_ms = 0;
    std::string player_id;
    std::uint16_t cmd = 0;
    std::vector<char> payload;
};

class ReplayRecorder {
public:
    // 追加一条事件；时间戳必须非降序，否则拒绝
    bool add(const ReplayEvent& e) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!events_.empty() && e.time_ms < events_.back().time_ms) return false;
        events_.push_back(e);
        return true;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return events_.size();
    }

    // 按时间区间取事件 [from_ms, to_ms]
    std::vector<ReplayEvent> range(std::int64_t from_ms, std::int64_t to_ms) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<ReplayEvent> out;
        for (const auto& e : events_) {
            if (e.time_ms >= from_ms && e.time_ms <= to_ms) out.push_back(e);
        }
        return out;
    }

    std::vector<ReplayEvent> all() const {
        std::lock_guard<std::mutex> lock(mu_);
        return events_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        events_.clear();
    }

private:
    mutable std::mutex mu_;
    std::vector<ReplayEvent> events_;
};

// 观战订阅：主播推送帧，订阅者拉取最新/增量
class SpectatorFeed {
public:
    struct Frame {
        std::int64_t seq = 0;
        std::int64_t time_ms = 0;
        std::vector<char> data;
    };

    // 推送新帧，返回 seq
    std::int64_t push(std::vector<char> data, std::int64_t time_ms) {
        std::lock_guard<std::mutex> lock(mu_);
        Frame f;
        f.seq = ++seq_;
        f.time_ms = time_ms;
        f.data = std::move(data);
        frames_.push_back(std::move(f));
        if (frames_.size() > max_frames_) {
            frames_.erase(frames_.begin(), frames_.begin() + (frames_.size() - max_frames_));
        }
        return seq_;
    }

    // 取大于 after_seq 的帧（增量同步）
    std::vector<Frame> take_after(std::int64_t after_seq) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Frame> out;
        for (const auto& f : frames_) {
            if (f.seq > after_seq) out.push_back(f);
        }
        return out;
    }

    // 最新一帧；无帧返回 false
    bool latest(Frame& out) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (frames_.empty()) return false;
        out = frames_.back();
        return true;
    }

    std::int64_t seq() const {
        std::lock_guard<std::mutex> lock(mu_);
        return seq_;
    }

    void set_max_frames(std::size_t n) {
        std::lock_guard<std::mutex> lock(mu_);
        max_frames_ = n < 1 ? 1 : n;
        if (frames_.size() > max_frames_) {
            frames_.erase(frames_.begin(), frames_.begin() + (frames_.size() - max_frames_));
        }
    }

private:
    mutable std::mutex mu_;
    std::int64_t seq_ = 0;
    std::size_t max_frames_ = 256;
    std::vector<Frame> frames_;
};

}  // namespace game
}  // namespace chwell
