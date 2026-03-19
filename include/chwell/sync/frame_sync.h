#pragma once

#include "chwell/service/component.h"
#include "chwell/service/service.h"
#include "chwell/service/session_manager.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/core/logger.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <cstdint>
#include <queue>
#include <mutex>

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

// 帧同步房间
class FrameSyncRoom {
public:
    FrameSyncRoom(const std::string& room_id, uint32_t frame_rate = 30)
        : room_id_(room_id), frame_rate_(frame_rate),
          current_frame_(0), running_(false) {}

    // 加入房间
    void join_player(uint32_t player_id, const net::TcpConnectionPtr& conn) {
        // std::lock_guard<std::mutex> lock(mutex_);
        // players_[player_id] = conn;
        player_inputs_[player_id] = std::queue<FrameInput>();
        // CHWELL_LOG_INFO("Player " + std::to_string(player_id) + " joined frame sync room " + room_id_);
    }

    // 离开房间
    void leave_player(uint32_t player_id) {
        // std::lock_guard<std::mutex> lock(mutex_);
        players_.erase(player_id);
        player_inputs_.erase(player_id);
        CHWELL_LOG_INFO("Player " + std::to_string(player_id) + " left frame sync room " + room_id_);
    }

    // 提交输入
    void submit_input(uint32_t player_id, const FrameInput& input) {
        // std::lock_guard<std::mutex> lock(mutex_);
        player_inputs_[player_id].push(input);
        CHWELL_LOG_DEBUG("Player " + std::to_string(player_id) + " submitted input for frame " + std::to_string(input.frame_id));
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

    // 获取当前帧
    uint32_t current_frame() const { return current_frame_; }

    // 推进帧
    void advance_frame() {
        std::lock_guard<std::mutex> lock(mutex_);
        current_frame_++;
        CHWELL_LOG_DEBUG("Advanced to frame " + std::to_string(current_frame_));
    }

    // 开始/停止同步
    void start_sync() { running_ = true; }
    void stop_sync() { running_ = false; }
    bool is_running() const { return running_; }

    // 获取房间 ID
    const std::string& room_id() const { return room_id_; }

    // 获取玩家数量
    size_t player_count() const {
        // std::lock_guard<std::mutex> lock(mutex_);
        return player_inputs_.size();  // 改用 player_inputs_.size()
    }

    // 获取所有玩家 ID
    std::vector<uint32_t> get_player_ids() {
        // std::lock_guard<std::mutex> lock(mutex_);
        std::vector<uint32_t> ids;
        for (auto& pair : player_inputs_) {
            ids.push_back(pair.first);
        }
        return ids;
    }

    // 检查是否所有玩家都提交了输入
    bool all_inputs_ready(uint32_t frame_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& pair : player_inputs_) {
            if (pair.second.empty() || pair.second.front().frame_id != frame_id) {
                return false;
            }
        }
        return true;
    }

private:
    std::string room_id_;
    uint32_t frame_rate_;
    uint32_t current_frame_;
    bool running_;

    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, net::TcpConnectionPtr> players_;
    std::unordered_map<uint32_t, std::queue<FrameInput>> player_inputs_;
    std::unordered_map<uint32_t, FrameSnapshot> snapshots_;
};

// 帧同步组件
class FrameSyncComponent : public service::Component {
public:
    FrameSyncComponent(uint32_t frame_rate = 30) : frame_rate_(frame_rate) {}

    virtual std::string name() const override { return "FrameSyncComponent"; }

    // 注册协议处理器
    virtual void on_register(service::Service& svc) override;

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

    // 获取连接所在的房间 ID
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
};

} // namespace sync
} // namespace chwell
