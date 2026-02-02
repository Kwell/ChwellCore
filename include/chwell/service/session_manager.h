#pragma once

#include <unordered_map>
#include <string>
#include <memory>
#include <vector>
#include <chrono>
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
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            core::Logger::instance().info(
                "Session removed, player_id=" + it->second.player_id +
                ", room_id=" + it->second.room_id);
            sessions_.erase(it);
        }
    }

    // 登录：绑定玩家ID
    void login(const net::TcpConnectionPtr& conn, const std::string& player_id) {
        SessionInfo& s = sessions_[conn.get()];
        s.player_id = player_id;
        s.authed = true;
        update_active_time(conn);
        core::Logger::instance().info("Player login, id=" + player_id);
    }

    // 登出
    void logout(const net::TcpConnectionPtr& conn) {
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            core::Logger::instance().info("Player logout, id=" + it->second.player_id);
            sessions_.erase(it);
        }
    }

    // 加入房间
    void join_room(const net::TcpConnectionPtr& conn, const std::string& room_id) {
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            it->second.room_id = room_id;
            update_active_time(conn);
            core::Logger::instance().info(
                "Player " + it->second.player_id + " join room " + room_id);
        }
    }

    // 离开房间
    void leave_room(const net::TcpConnectionPtr& conn) {
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            std::string room_id = it->second.room_id;
            it->second.room_id.clear();
            update_active_time(conn);
            core::Logger::instance().info(
                "Player " + it->second.player_id + " leave room " + room_id);
        }
    }

    // 设置网关ID
    void set_gateway(const net::TcpConnectionPtr& conn, const std::string& gateway_id) {
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            it->second.gateway_id = gateway_id;
            update_active_time(conn);
        }
    }

    // 查询接口
    bool is_logged_in(const net::TcpConnectionPtr& conn) const {
        auto it = sessions_.find(conn.get());
        return it != sessions_.end() && it->second.authed;
    }

    std::string get_player_id(const net::TcpConnectionPtr& conn) const {
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end() && it->second.authed) {
            return it->second.player_id;
        }
        return std::string();
    }

    std::string get_room_id(const net::TcpConnectionPtr& conn) const {
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            return it->second.room_id;
        }
        return std::string();
    }

    // 获取房间内所有连接的玩家ID列表
    std::vector<std::string> get_players_in_room(const std::string& room_id) const {
        std::vector<std::string> players;
        for (const auto& pair : sessions_) {
            if (pair.second.room_id == room_id && pair.second.authed) {
                players.push_back(pair.second.player_id);
            }
        }
        return players;
    }

    // 更新活跃时间
    void update_active_time(const net::TcpConnectionPtr& conn) {
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            // 简单实现：使用秒级时间戳
            it->second.last_active_time = 
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
        }
    }

private:
    std::unordered_map<const net::TcpConnection*, SessionInfo> sessions_;
};

} // namespace service
} // namespace chwell
