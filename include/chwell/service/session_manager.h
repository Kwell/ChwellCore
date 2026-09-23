#pragma once

#include <algorithm>
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>
#include <chrono>
#include <shared_mutex>
#include "chwell/service/component.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace service {

// 增强的会话信息：支持玩家ID、房间ID、网关ID等
struct SessionInfo {
    std::string player_id;
    std::string room_id;
    std::string gateway_id;
    bool authed;
    std::int64_t last_active_time; // 最后活跃时间戳（秒）

    SessionInfo() : authed(false), last_active_time(0) {}
};

// SessionManager：增强的会话管理组件
// 支持玩家ID、房间ID、网关ID绑定，以及按各种维度查询
class SessionManager : public Component {
public:
    virtual std::string name() const override {
        return "SessionManager";
    }

    virtual void on_disconnect(const net::TcpConnectionPtr& conn) override {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn->conn_id());
        if (it != sessions_.end()) {
            CHWELL_LOG_INFO(
                "Session removed, player_id=" + it->second.player_id +
                ", room_id=" + it->second.room_id);
            remove_from_room_index(it->second, conn->conn_id());
            sessions_.erase(it);
        }
    }

    // 登录：绑定玩家ID
    void login(const net::TcpConnectionPtr& conn, const std::string& player_id) {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn->conn_id());
        if (it != sessions_.end()) {
            // Remove from old room index if already in a room
            remove_from_room_index(it->second, conn->conn_id());
        }
        SessionInfo& s = sessions_[conn->conn_id()];
        s.player_id = player_id;
        s.authed = true;
        update_active_time(s);
        CHWELL_LOG_INFO("Player login, id=" + player_id);
    }

    // 登出
    void logout(const net::TcpConnectionPtr& conn) {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn->conn_id());
        if (it != sessions_.end()) {
            remove_from_room_index(it->second, conn->conn_id());
            // CHWELL_LOG_INFO("Player logout, id=" + it->second.player_id);
            sessions_.erase(it);
        }
    }

    // 加入房间
    void join_room(const net::TcpConnectionPtr& conn, const std::string& room_id) {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn->conn_id());
        if (it != sessions_.end()) {
            // Remove from old room index if switching rooms
            remove_from_room_index(it->second, conn->conn_id());
            it->second.room_id = room_id;
            // Add to new room index
            if (!room_id.empty() && it->second.authed) {
                room_players_[room_id].push_back(conn->conn_id());
            }
            update_active_time(it->second);
            // CHWELL_LOG_INFO(
            //     "Player " + it->second.player_id + " join room " + room_id);
        }
    }

    // 离开房间
    void leave_room(const net::TcpConnectionPtr& conn) {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn->conn_id());
        if (it != sessions_.end()) {
            std::string room_id = it->second.room_id;
            remove_from_room_index(it->second, conn->conn_id());
            it->second.room_id.clear();
            update_active_time(it->second);
            // CHWELL_LOG_INFO(
            //     "Player " + it->second.player_id + " leave room " + room_id);
        }
    }

    // 设置网关ID
    void set_gateway(const net::TcpConnectionPtr& conn, const std::string& gateway_id) {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn->conn_id());
        if (it != sessions_.end()) {
            it->second.gateway_id = gateway_id;
            update_active_time(it->second);
        }
    }

    // 查询接口
    bool is_logged_in(const net::TcpConnectionPtr& conn) const {
        std::shared_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn->conn_id());
        return it != sessions_.end() && it->second.authed;
    }

    std::string get_player_id(const net::TcpConnectionPtr& conn) const {
        std::shared_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn->conn_id());
        if (it != sessions_.end() && it->second.authed) {
            return it->second.player_id;
        }
        return std::string();
    }

    std::string get_room_id(const net::TcpConnectionPtr& conn) const {
        std::shared_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn->conn_id());
        if (it != sessions_.end()) {
            return it->second.room_id;
        }
        return std::string();
    }

    // 获取房间内所有连接的玩家ID列表 (O(1) via reverse index)
    std::vector<std::string> get_players_in_room(const std::string& room_id) const {
        std::vector<std::string> players;
        std::shared_lock lock(sessions_mutex_);
        auto rit = room_players_.find(room_id);
        if (rit != room_players_.end()) {
            players.reserve(rit->second.size());
            for (const auto conn_id : rit->second) {
                auto it = sessions_.find(conn_id);
                if (it != sessions_.end() && it->second.authed) {
                    players.push_back(it->second.player_id);
                }
            }
        }
        return players;
    }

    // 更新活跃时间（内部使用）
    void update_active_time(SessionInfo& info) {
        info.last_active_time =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // 设置会话超时时间（秒），超过此时间未活跃的会话将被清理
    void set_session_timeout(int seconds) {
        std::unique_lock lock(sessions_mutex_);
        session_timeout_sec_ = seconds;
    }

    // 清理过期会话（应在 Update() 中周期调用）
    // 返回清理的会话数量
    size_t cleanup_expired_sessions() {
        std::unique_lock lock(sessions_mutex_);
        if (session_timeout_sec_ <= 0) return 0;

        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        size_t cleaned = 0;
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            if (now - it->second.last_active_time > session_timeout_sec_) {
                CHWELL_LOG_INFO("Session expired, player_id=" + it->second.player_id
                                + ", room_id=" + it->second.room_id);
                remove_from_room_index(it->second, it->first);
                it = sessions_.erase(it);
                ++cleaned;
            } else {
                ++it;
            }
        }
        return cleaned;
    }

    // 组件 Update 周期调用，自动清理过期会话
    virtual bool Update(int64_t /*delta_ms*/) override {
        cleanup_expired_sessions();
        return true;
    }

private:
    void remove_from_room_index(const SessionInfo& info, std::uint64_t conn_id) {
        if (!info.room_id.empty()) {
            auto rit = room_players_.find(info.room_id);
            if (rit != room_players_.end()) {
                auto& vec = rit->second;
                vec.erase(std::remove(vec.begin(), vec.end(), conn_id), vec.end());
                if (vec.empty()) {
                    room_players_.erase(rit);
                }
            }
        }
    }

    std::unordered_map<std::uint64_t, SessionInfo> sessions_;
    // Reverse index: room_id -> list of connection ids in that room
    std::unordered_map<std::string, std::vector<std::uint64_t>> room_players_;
    int session_timeout_sec_ = 1800;  // 会话超时（秒），默认 30 分钟
    mutable std::shared_mutex sessions_mutex_;
};

} // namespace service
} // namespace chwell
