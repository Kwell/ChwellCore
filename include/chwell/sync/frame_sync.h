#pragma once

#include "chwell/service/component.h"
#include "chwell/service/service.h"
#include "chwell/service/session_manager.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/core/logger.h"
#include "chwell/core/timer_wheel.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <cstdint>
#include <queue>
#include <mutex>
#include <atomic>
#include <chrono>
#include <map>

namespace chwell {
namespace sync {

// ============================================
// 帧同步协议命令字
// ============================================

namespace frame_cmd {
    const uint16_t C2S_FRAME_INPUT      = 0x0101;
    const uint16_t C2S_FRAME_SYNC_REQ   = 0x0102;
    const uint16_t S2C_FRAME_SYNC       = 0x0103;
    const uint16_t S2C_FRAME_STATE      = 0x0104;
    const uint16_t S2C_FRAME_SNAPSHOT   = 0x0105;
    const uint16_t S2C_FRAME_ERROR      = 0x01FF;
}

// ============================================
// 帧同步组件
// ============================================

// 帧输入数据
struct FrameInput {
    uint32_t frame_id;
    uint32_t player_id;
    std::vector<uint8_t> input_data;
};

// 帧状态
struct FrameState {
    uint32_t frame_id;
    std::vector<uint8_t> state_data;
};

// 帧快照（用于回滚）
struct FrameSnapshot {
    uint32_t frame_id;
    std::vector<uint8_t> snapshot_data;
};

/**
 * @brief 帧同步房间
 *
 * 改进（P0-3）：
 * - 帧超时兜底：如果某个玩家未提交输入超过阈值，填充空输入并强制推进
 * - 绝不让全网等一人
 */
class FrameSyncRoom {
public:
    FrameSyncRoom(const std::string& room_id, uint32_t frame_rate = 30)
        : room_id_(room_id), frame_rate_(frame_rate),
          current_frame_(0), running_(false),
          frame_timeout_ms_(1000 / frame_rate * 2)  // 默认 2 帧时间超时
    {
        last_advance_time_ = std::chrono::steady_clock::now();
    }

    // 加入房间
    void join_player(uint32_t player_id, const net::TcpConnectionPtr& conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        players_[player_id] = conn;
        player_inputs_[player_id] = std::queue<FrameInput>();
        // 🆕 初始化已提交帧集合
        submitted_frames_[player_id] = std::unordered_set<uint32_t>();
        CHWELL_LOG_INFO("Player " + std::to_string(player_id) + " joined frame sync room " + room_id_);
    }

    // 离开房间
    void leave_player(uint32_t player_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        players_.erase(player_id);
        player_inputs_.erase(player_id);
        submitted_frames_.erase(player_id);
        CHWELL_LOG_INFO("Player " + std::to_string(player_id) + " left frame sync room " + room_id_);
    }

    // 提交输入
    void submit_input(uint32_t player_id, const FrameInput& input) {
        std::lock_guard<std::mutex> lock(mutex_);
        player_inputs_[player_id].push(input);
        // 🆕 记录已提交的帧
        submitted_frames_[player_id].insert(input.frame_id);
        // 修剪过旧帧记录，防止集合无限增长
        {
            uint32_t cur = current_frame_.load(std::memory_order_relaxed);
            auto& s = submitted_frames_[player_id];
            for (auto it = s.begin(); it != s.end();) {
                if (*it + 256 < cur) it = s.erase(it);
                else ++it;
            }
            if (s.empty()) submitted_frames_.erase(player_id);
        }
        // 限制单玩家输入队列长度
        auto& q = player_inputs_[player_id];
        while (q.size() > 256) q.pop();
        CHWELL_LOG_DEBUG("Player " + std::to_string(player_id)
                         + " submitted input for frame " + std::to_string(input.frame_id));
    }

    // 获取所有输入（用于游戏逻辑）
    std::vector<FrameInput> get_all_inputs(uint32_t frame_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<FrameInput> inputs;

        for (auto& pair : player_inputs_) {
            auto& queue = pair.second;
            while (!queue.empty() && queue.front().frame_id <= frame_id) {
                if (queue.front().frame_id == frame_id) {
                    inputs.push_back(queue.front());
                }
                queue.pop();
            }
        }

        return inputs;
    }

    // 创建快照
    void create_snapshot(const FrameSnapshot& snapshot) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshots_[snapshot.frame_id] = snapshot;
        // 只保留最近的 10 个快照
        // 使用 std::map 按 frame_id 排序，begin() 即为最小（最旧）的帧
        while (snapshots_.size() > 10) {
            snapshots_.erase(snapshots_.begin());
        }
        CHWELL_LOG_DEBUG("Created snapshot for frame " + std::to_string(snapshot.frame_id));
    }

    // 获取快照
    bool get_snapshot(uint32_t frame_id, FrameSnapshot& snapshot) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = snapshots_.find(frame_id);
        if (it != snapshots_.end()) {
            snapshot = it->second;
            return true;
        }
        return false;
    }

    // 获取当前帧（原子读取，无需加锁）
    uint32_t current_frame() const {
        return current_frame_.load(std::memory_order_acquire);
    }

    // 推进帧
    void advance_frame() {
        std::lock_guard<std::mutex> lock(mutex_);
        current_frame_.fetch_add(1, std::memory_order_release);
        last_advance_time_ = std::chrono::steady_clock::now();
    }

    // 开始/停止同步（原子写入）
    void start_sync() { running_.store(true, std::memory_order_release); }
    void stop_sync() { running_.store(false, std::memory_order_release); }
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // 获取房间 ID
    const std::string& room_id() const { return room_id_; }

    // 获取玩家数量
    size_t player_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return player_inputs_.size();
    }

    // 获取所有玩家 ID
    std::vector<uint32_t> get_player_ids() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<uint32_t> ids;
        for (auto& pair : player_inputs_) {
            ids.push_back(pair.first);
        }
        return ids;
    }

    // 获取所有玩家的连接（用于广播）
    std::vector<net::TcpConnectionPtr> get_player_connections() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<net::TcpConnectionPtr> conns;
        conns.reserve(players_.size());
        for (const auto& pair : players_) {
            conns.push_back(pair.second);
        }
        return conns;
    }

    // 检查是否所有玩家都提交了输入
    bool all_inputs_ready(uint32_t frame_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& pair : submitted_frames_) {
            if (pair.second.find(frame_id) == pair.second.end()) {
                return false;
            }
        }
        return true;
    }

    // ========== 🆕 帧超时兜底（P0-3） ==========

    // 设置帧超时（毫秒，原子写入）
    void set_frame_timeout(uint32_t timeout_ms) {
        frame_timeout_ms_.store(timeout_ms, std::memory_order_release);
    }
    uint32_t frame_timeout_ms() const {
        return frame_timeout_ms_.load(std::memory_order_acquire);
    }

    /**
     * @brief 检查并推进超时帧
     *
     * 如果当前帧等待超过 frame_timeout_ms_，为未提交的玩家填充空输入并强制推进。
     * 由定时器周期调用（在 Logic Thread 中执行，无需额外加锁）。
     *
     * @return true 执行了超时推进，false 未超时
     */
    bool check_frame_timeout() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load(std::memory_order_acquire) || player_inputs_.empty()) return false;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_advance_time_).count();

        if (elapsed < static_cast<int64_t>(frame_timeout_ms_.load(std::memory_order_acquire)))
            return false;

        // 超时了，为未提交当前帧输入的玩家填充空输入
        uint32_t cur_frame = current_frame_.load(std::memory_order_relaxed);
        for (auto& pair : submitted_frames_) {
            uint32_t player_id = pair.first;
            auto& frames = pair.second;

            if (frames.find(cur_frame) == frames.end()) {
                // 该玩家未提交当前帧输入，填充空输入
                FrameInput empty_input;
                empty_input.frame_id = cur_frame;
                empty_input.player_id = player_id;
                empty_input.input_data.clear();
                player_inputs_[player_id].push(empty_input);
                frames.insert(cur_frame);

                CHWELL_LOG_WARN("Frame timeout: filling empty input for player "
                                + std::to_string(player_id)
                                + " frame=" + std::to_string(cur_frame)
                                + " room=" + room_id_);
            }
        }

        // 强制推进
        current_frame_.fetch_add(1, std::memory_order_release);
        last_advance_time_ = now;
        return true;
    }

    // 获取最近一次推进时间
    std::chrono::steady_clock::time_point last_advance_time() const {
        return last_advance_time_;
    }

private:
    std::string room_id_;
    uint32_t frame_rate_;
    std::atomic<uint32_t> current_frame_;   // 原子类型，支持无锁读取
    std::atomic<bool> running_;             // 原子类型，支持无锁读取

    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, net::TcpConnectionPtr> players_;
    std::unordered_map<uint32_t, std::queue<FrameInput>> player_inputs_;
    std::map<uint32_t, FrameSnapshot> snapshots_;  // 改用 std::map 按 frame_id 排序
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> submitted_frames_;

    // 帧超时相关
    std::atomic<uint32_t> frame_timeout_ms_;       // 原子类型，支持无锁读取
    std::chrono::steady_clock::time_point last_advance_time_;
};

/**
 * @brief 帧同步组件
 *
 * 改进（P0-3）：
 * - PreUpdate 中启动帧超时检测定时器
 * - Shut 中取消定时器
 */
class FrameSyncComponent : public service::Component {
public:
    FrameSyncComponent(uint32_t frame_rate = 30) : frame_rate_(frame_rate) {}

    virtual std::string name() const override { return "FrameSyncComponent"; }

    // 注册协议处理器
    virtual void on_register(service::Service& svc) override;

    // 🆕 启动帧超时检测定时器
    virtual bool PreUpdate() override;

    // 🆕 取消定时器
    virtual bool Shut() override;

    // 处理帧输入
    void handle_frame_input(const net::TcpConnectionPtr& conn, const std::vector<char>& data);

    // 处理帧同步请求
    void handle_frame_sync_req(const net::TcpConnectionPtr& conn, const std::vector<char>& data);

    // 创建房间
    void create_room(const std::string& room_id);

    // 销毁房间
    void destroy_room(const std::string& room_id);

    // 加入房间
    void join_room(uint32_t player_id, const std::string& room_id, const net::TcpConnectionPtr& conn);

    // 离开房间
    void leave_room(uint32_t player_id, const std::string& room_id);

    // 提交输入
    void submit_input(uint32_t player_id, const FrameInput& input);

    // 创建快照
    void create_snapshot(const std::string& room_id, const FrameSnapshot& snapshot);

    // 广播帧状态
    void broadcast_frame_state(const std::string& room_id, const FrameState& state);

    // 发送帧同步
    void send_frame_sync(const net::TcpConnectionPtr& conn, uint32_t current_frame);

    // 发送帧快照
    void send_frame_snapshot(const net::TcpConnectionPtr& conn, const FrameSnapshot& snapshot);

    // 连接断开时自动离开房间
    virtual void on_disconnect(const net::TcpConnectionPtr& conn) override;

private:
    // 获取 SessionManager
    service::SessionManager* get_session_manager();

    // 从连接获取玩家 ID
    uint32_t get_player_id(const net::TcpConnectionPtr& conn);

    // 从连接获取所在的房间 ID
    std::string get_room_id(const net::TcpConnectionPtr& conn);

    // 维护连接到房间的映射
    struct ConnectionInfo {
        uint32_t player_id;
        std::string room_id;
    };

    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<FrameSyncRoom>> rooms_;
    std::unordered_map<net::TcpConnection*, ConnectionInfo> connections_;
    uint32_t frame_rate_;

    // 🆕 帧超时检测定时器句柄
    core::TimerHandle frame_timer_handle_;
};

} // namespace sync
} // namespace chwell
