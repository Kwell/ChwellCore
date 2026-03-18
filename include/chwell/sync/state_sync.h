#pragma once

#include "chwell/service/component.h"
#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/service/session_manager.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/protocol/message.h"
#include "chwell/core/logger.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <functional>

namespace chwell {
namespace sync {

// ============================================
// 状态同步协议命令字
// ============================================

namespace state_cmd {
    const uint16_t C2S_STATE_UPDATE    = 0x0201;
    const uint16_t C2S_STATE_QUERY     = 0x0202;
    const uint16_t C2S_STATE_SUBSCRIBE = 0x0203;
    const uint16_t C2S_STATE_UNSUBSCRIBE = 0x0204;
    const uint16_t S2C_STATE_UPDATE    = 0x0205;
    const uint16_t S2C_STATE_DIFF      = 0x0206;
    const uint16_t S2C_STATE_SNAPSHOT  = 0x0207;
    const uint16_t S2C_STATE_ERROR     = 0x02FF;
}

// ============================================
// 状态数据类型
// ============================================

// 状态值（支持不同类型）
enum class StateValueType {
    INT32,
    INT64,
    FLOAT,
    DOUBLE,
    STRING,
    BINARY
};

// 状态值
struct StateValue {
    StateValueType type;
    std::string value; // 统一用 string 存储，根据 type 解析

    StateValue() : type(StateValueType::INT32) {}

    StateValue(int32_t v) : type(StateValueType::INT32), value(reinterpret_cast<const char*>(&v), sizeof(int32_t)) {}
    StateValue(int64_t v) : type(StateValueType::INT64), value(reinterpret_cast<const char*>(&v), sizeof(int64_t)) {}
    StateValue(float v) : type(StateValueType::FLOAT), value(reinterpret_cast<const char*>(&v), sizeof(float)) {}
    StateValue(double v) : type(StateValueType::DOUBLE), value(reinterpret_cast<const char*>(&v), sizeof(double)) {}
    StateValue(const std::string& v) : type(StateValueType::STRING), value(v) {}
    StateValue(const std::vector<uint8_t>& v) : type(StateValueType::BINARY), value(v.begin(), v.end()) {}

    int32_t as_int32() const {
        if (type != StateValueType::INT32 || value.size() != sizeof(int32_t)) return 0;
        int32_t result;
        std::memcpy(&result, value.data(), sizeof(int32_t));
        return result;
    }

    int64_t as_int64() const {
        if (type != StateValueType::INT64 || value.size() != sizeof(int64_t)) return 0;
        int64_t result;
        std::memcpy(&result, value.data(), sizeof(int64_t));
        return result;
    }

    float as_float() const {
        if (type != StateValueType::FLOAT || value.size() != sizeof(float)) return 0.0f;
        float result;
        std::memcpy(&result, value.data(), sizeof(float));
        return result;
    }

    double as_double() const {
        if (type != StateValueType::DOUBLE || value.size() != sizeof(double)) return 0.0;
        double result;
        std::memcpy(&result, value.data(), sizeof(double));
        return result;
    }

    std::string as_string() const {
        if (type == StateValueType::STRING) {
            return value;
        }
        return "";
    }

    std::vector<uint8_t> as_binary() const {
        if (type == StateValueType::BINARY) {
            return std::vector<uint8_t>(value.begin(), value.end());
        }
        return {};
    }
};

// 状态更新
struct StateUpdate {
    std::string entity_id;  // 实体 ID
    std::string state_key;  // 状态键
    StateValue value;       // 状态值
    uint64_t timestamp;     // 时间戳
};

// 状态差异（用于增量更新）
struct StateDiff {
    std::string entity_id;
    std::vector<std::pair<std::string, StateValue>> changes; // 状态键 -> 状态值
    uint64_t timestamp;
};

// 状态快照（完整状态）
struct StateSnapshot {
    std::string entity_id;
    std::unordered_map<std::string, StateValue> states; // 状态键 -> 状态值
    uint64_t timestamp;
};

// 状态订阅回调
using StateUpdateCallback = std::function<void(const StateUpdate&)>;

// ============================================
// 状态同步房间
// ============================================

class StateSyncRoom {
public:
    StateSyncRoom(const std::string& room_id) : room_id_(room_id) {}

    // 设置发送差异的回调
    void set_diff_callback(std::function<void(const net::TcpConnectionPtr&, const StateDiff&)> callback) {
        diff_callback_ = callback;
    }

    // 设置发送快照的回调
    void set_snapshot_callback(std::function<void(const net::TcpConnectionPtr&, const StateSnapshot&)> callback) {
        snapshot_callback_ = callback;
    }

    // 更新状态
    void update_state(const StateUpdate& update) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 更新实体状态
        auto& entity_states = states_[update.entity_id];
        entity_states[update.state_key] = update.value;

        // 记录时间戳
        timestamps_[update.entity_id] = update.timestamp;

        // 创建状态差异
        StateDiff diff;
        diff.entity_id = update.entity_id;
        diff.changes.push_back({update.state_key, update.value});
        diff.timestamp = update.timestamp;

        // 通知订阅者
        if (diff_callback_) {
            auto subs_it = subscribers_.find(update.entity_id);
            if (subs_it != subscribers_.end()) {
                for (auto* raw_conn : subs_it->second) {
                    // 需要将raw_ptr转换为shared_ptr
                    // 这里简化处理，实际需要维护连接映射
                    auto conn_it = connection_map_.find(raw_conn);
                    if (conn_it != connection_map_.end()) {
                        diff_callback_(conn_it->second, diff);
                    }
                }
            }
        }
    }

    // 查询状态
    bool query_state(const std::string& entity_id, const std::string& state_key, StateValue& out) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto entity_it = states_.find(entity_id);
        if (entity_it == states_.end()) {
            return false;
        }

        auto key_it = entity_it->second.find(state_key);
        if (key_it == entity_it->second.end()) {
            return false;
        }

        out = key_it->second;
        return true;
    }

    // 查询所有状态
    bool query_all_states(const std::string& entity_id, std::unordered_map<std::string, StateValue>& out) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = states_.find(entity_id);
        if (it == states_.end()) {
            return false;
        }

        out = it->second;
        return true;
    }

    // 创建快照
    StateSnapshot create_snapshot(const std::string& entity_id) {
        std::lock_guard<std::mutex> lock(mutex_);

        StateSnapshot snapshot;
        snapshot.entity_id = entity_id;

        auto it = states_.find(entity_id);
        if (it != states_.end()) {
            snapshot.states = it->second;
        }

        auto ts_it = timestamps_.find(entity_id);
        if (ts_it != timestamps_.end()) {
            snapshot.timestamp = ts_it->second;
        }

        return snapshot;
    }

    // 订阅状态更新
    void subscribe(const std::string& entity_id, const net::TcpConnectionPtr& conn) {
        std::lock_guard<std::mutex> lock(mutex_);

        subscribers_[entity_id].insert(conn.get());

        // 保存连接映射
        connection_map_[conn.get()] = conn;

        // 发送当前状态快照
        if (snapshot_callback_) {
            StateSnapshot snapshot = create_snapshot(entity_id);
            snapshot_callback_(conn, snapshot);
        }
    }

    // 取消订阅
    void unsubscribe(const std::string& entity_id, const net::TcpConnectionPtr& conn) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = subscribers_.find(entity_id);
        if (it != subscribers_.end()) {
            it->second.erase(conn.get());
        }
    }

    // 获取房间 ID
    const std::string& room_id() const { return room_id_; }

    // 获取实体状态
    bool get_entity_states(const std::string& entity_id, std::unordered_map<std::string, StateValue>& out) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = states_.find(entity_id);
        if (it != states_.end()) {
            out = it->second;
            return true;
        }

        return false;
    }

    // 获取实体时间戳
    bool get_entity_timestamp(const std::string& entity_id, uint64_t& out) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = timestamps_.find(entity_id);
        if (it != timestamps_.end()) {
            out = it->second;
            return true;
        }

        return false;
    }

    // 获取实体订阅者
    std::unordered_set<net::TcpConnection*> get_entity_subscribers(const std::string& entity_id) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = subscribers_.find(entity_id);
        if (it != subscribers_.end()) {
            return it->second;
        }

        return {};
    }

private:
    std::string room_id_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_map<std::string, StateValue>> states_; // entity_id -> state_key -> value
    std::unordered_map<std::string, uint64_t> timestamps_; // entity_id -> timestamp
    std::unordered_map<std::string, std::unordered_set<net::TcpConnection*>> subscribers_; // entity_id -> connections
    std::unordered_map<net::TcpConnection*, net::TcpConnectionPtr> connection_map_; // raw_ptr -> shared_ptr
    std::function<void(const net::TcpConnectionPtr&, const StateDiff&)> diff_callback_;
    std::function<void(const net::TcpConnectionPtr&, const StateSnapshot&)> snapshot_callback_;
};

// ============================================
// 状态同步组件
// ============================================

class StateSyncComponent : public service::Component {
public:
    StateSyncComponent() = default;

    virtual std::string name() const override { return "StateSyncComponent"; }

    // 注册协议处理器
    virtual void on_register(service::Service& svc) override;

    // 处理状态更新
    void handle_state_update(const net::TcpConnectionPtr& conn, const std::vector<char>& data);

    // 处理状态查询
    void handle_state_query(const net::TcpConnectionPtr& conn, const std::vector<char>& data);

    // 处理状态订阅
    void handle_state_subscribe(const net::TcpConnectionPtr& conn, const std::vector<char>& data);

    // 处理取消订阅
    void handle_state_unsubscribe(const net::TcpConnectionPtr& conn, const std::vector<char>& data);

    // 创建房间
    void create_room(const std::string& room_id);

    // 销毁房间
    void destroy_room(const std::string& room_id);

    // 更新状态
    void update_state(const std::string& room_id, const StateUpdate& update);

    // 查询状态
    bool query_state(const std::string& room_id, const std::string& entity_id, const std::string& state_key, StateValue& out);

    // 查询所有状态
    bool query_all_states(const std::string& room_id, const std::string& entity_id, std::unordered_map<std::string, StateValue>& out);

    // 创建快照
    StateSnapshot create_snapshot(const std::string& room_id, const std::string& entity_id);

    // 连接断开时自动取消订阅
    virtual void on_disconnect(const net::TcpConnectionPtr& conn) override;

private:
    // 发送状态差异
    void send_state_diff(const net::TcpConnectionPtr& conn, const StateDiff& diff);

    // 发送状态快照
    void send_state_snapshot(const net::TcpConnectionPtr& conn, const StateSnapshot& snapshot);

    // 获取 SessionManager
    service::SessionManager* get_session_manager();

    // 获取房间 ID
    std::string get_room_id(const net::TcpConnectionPtr& conn);

    service::Service* service_ = nullptr;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<StateSyncRoom>> rooms_;
    std::unordered_map<net::TcpConnection*, std::string> connections_; // connection -> room_id
};

} // namespace sync
} // namespace chwell
