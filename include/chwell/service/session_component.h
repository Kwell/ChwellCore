#pragma once

#include <unordered_map>
#include <string>

#include "chwell/service/component.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace service {

// 简单的会话信息结构（非持久化，仅进程内）
struct SessionInfo {
    std::string player_id;
    bool authed;

    SessionInfo() : authed(false) {}
};

// SessionComponent：负责管理连接与玩家会话的绑定
class SessionComponent : public Component {
public:
    virtual std::string name() const override {
        return "SessionComponent";
    }

    // 连接断开时，清理该连接的会话
    virtual void on_disconnect(const net::TcpConnectionPtr& conn) override {
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            CHWELL_LOG_INFO(
                "Session removed on disconnect, player_id=" + it->second.player_id);
            sessions_.erase(it);
        }
    }

    // 绑定 / 登录一个玩家
    void login(const net::TcpConnectionPtr& conn, const std::string& player_id) {
        SessionInfo& s = sessions_[conn.get()];
        s.player_id = player_id;
        s.authed = true;
        CHWELL_LOG_INFO("Player login, id=" + player_id);
    }

    // 登出一个玩家
    void logout(const net::TcpConnectionPtr& conn) {
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end()) {
            CHWELL_LOG_INFO(
                "Player logout, id=" + it->second.player_id);
            sessions_.erase(it);
        }
    }

    // 查询当前连接是否已登录
    bool is_logged_in(const net::TcpConnectionPtr& conn) const {
        auto it = sessions_.find(conn.get());
        return it != sessions_.end() && it->second.authed;
    }

    // 获取玩家ID（未登录时返回空串）
    std::string get_player_id(const net::TcpConnectionPtr& conn) const {
        auto it = sessions_.find(conn.get());
        if (it != sessions_.end() && it->second.authed) {
            return it->second.player_id;
        }
        return std::string();
    }

private:
    std::unordered_map<const net::TcpConnection*, SessionInfo> sessions_;
};

} // namespace service
} // namespace chwell

