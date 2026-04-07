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
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            CHWELL_LOG_INFO(
                "Session removed, player_id=" + it->second.player_id +
                ", room_id=" + it->second.room_id);
            remove_from_room_index(it->second, conn.get());
            sessions_.erase(it);
        }
    }

    // 登录：绑定玩家ID
    void login(const net::TcpConnectionPtr& conn, const std::string& player_id) {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            // Remove from old room index if already in a room
            remove_from_room_index(it->second, conn.get());
        }
        SessionInfo& s = sessions_[conn.get()];
        s.player_id = player_id;
        s.authed = true;
        update_active_time(s);
        CHWELL_LOG_INFO("Player login, id=" + player_id);
    }

    // 登出
    void logout(const net::TcpConnectionPtr& conn) {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            remove_from_room_index(it->second, conn.get());
            // CHWELL_LOG_INFO("Player logout, id=" + it->second.player_id);
            sessions_.erase(it);
        }
    }

    // 加入房间
    void join_room(const net::TcpConnectionPtr& conn, const std::string& room_id) {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            // Remove from old room index if switching rooms
            remove_from_room_index(it->second, conn.get());
            it->second.room_id = room_id;
            // Add to new room index
            if (!room_id.empty() && it->second.authed) {
                room_players_[room_id].push_back(conn.get());
            }
            update_active_time(it->second);
            // CHWELL_LOG_INFO(
            //     "Player " + it->second.player_id + " join room " + room_id);
        }
    }

    // 离开房间
    void leave_room(const net::TcpConnectionPtr& conn) {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            std::string room_id = it->second.room_id;
            remove_from_room_index(it->second, conn.get());
            it->second.room_id.clear();
            update_active_time(it->second);
            // CHWELL_LOG_INFO(
            //     "Player " + it->second.player_id + " leave room " + room_id);
        }
    }

    // 设置网关ID
    void set_gateway(const net::TcpConnectionPtr& conn, const std::string& gateway_id) {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            it->second.gateway_id = gateway_id;
            update_active_time(it->second);
        }
    }

    // 查询接口
    bool is_logged_in(const net::TcpConnectionPtr& conn) const {
        std::shared_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn.get());
        return it != sessions_.end() && it->second.authed;
    }

    std::string get_player_id(const net::TcpConnectionPtr& conn) const {
        std::shared_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end() && it->second.authed) {
            return it->second.player_id;
        }
        return std::string();
    }

    std::string get_room_id(const net::TcpConnectionPtr& conn) const {
        std::shared_lock lock(sessions_mutex_);
        auto it = sessions_.find(conn.get());
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
            for (const auto* conn_ptr : rit->second) {
                auto it = sessions_.find(conn_ptr);
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

private:
    void remove_from_room_index(const SessionInfo& info, const net::TcpConnection* conn) {
        if (!info.room_id.empty()) {
            auto rit = room_players_.find(info.room_id);
            if (rit != room_players_.end()) {
                auto& vec = rit->second;
                vec.erase(std::remove(vec.begin(), vec.end(), conn), vec.end());
                if (vec.empty()) {
                    room_players_.erase(rit);
                }
            }
        }
    }

    std::unordered_map<const net::TcpConnection*, SessionInfo> sessions_;
    // Reverse index: room_id -> list of connections in that room
    std::unordered_map<std::string, std::vector<const net::TcpConnection*>> room_players_;
    mutable std::shared_mutex sessions_mutex_;
};

} // namespace service
} // namespace chwell
