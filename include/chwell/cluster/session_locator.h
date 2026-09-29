#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "chwell/cluster/node_registry.h"

namespace chwell {
namespace cluster {

// ========== 跨服会话定位 ==========
//
// SessionLocator：维护「会话在哪个节点上」的全局视图，供网关/跨服消息路由。
// - bind：会话建立或迁移完成时登记
// - unbind：会话下线时清理
// - locate：查会话所在节点（供 RpcRouter::forward_to_node 使用）
// - migrate：旧节点解绑、新节点绑定（两步非原子，调用方保证顺序）
//
// 本类只做位置索引，不做会话状态同步；状态仍归各节点 SessionManager 所有。

struct SessionLocation {
    std::string session_id;
    std::string node_id;
    std::string node_type;   // 如 "gate" / "logic"
    std::int64_t bind_time_ms = 0;
};

class SessionLocator {
public:
    bool bind(const std::string& session_id,
              const std::string& node_id,
              const std::string& node_type = std::string(),
              std::int64_t now_ms = 0) {
        if (session_id.empty() || node_id.empty()) return false;
        std::lock_guard<std::mutex> lock(mu_);
        SessionLocation loc;
        loc.session_id = session_id;
        loc.node_id = node_id;
        loc.node_type = node_type;
        loc.bind_time_ms = now_ms;
        sessions_[session_id] = loc;
        return true;
    }

    bool unbind(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(mu_);
        return sessions_.erase(session_id) > 0;
    }

    bool locate(const std::string& session_id, SessionLocation& out) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return false;
        out = it->second;
        return true;
    }

    std::string node_of(const std::string& session_id) const {
        SessionLocation loc;
        if (!locate(session_id, loc)) return std::string();
        return loc.node_id;
    }

    // 迁移：会话从 old_node 挪到 new_node。若当前不在 old_node 则返回 false 且不改动。
    bool migrate(const std::string& session_id,
                 const std::string& old_node,
                 const std::string& new_node,
                 const std::string& node_type = std::string(),
                 std::int64_t now_ms = 0) {
        if (session_id.empty() || new_node.empty()) return false;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return false;
        if (!old_node.empty() && it->second.node_id != old_node) return false;
        it->second.node_id = new_node;
        if (!node_type.empty()) it->second.node_type = node_type;
        it->second.bind_time_ms = now_ms;
        return true;
    }

    // 节点下线时批量清除其上会话；返回清除条数
    std::size_t drop_node(const std::string& node_id) {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t n = 0;
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->second.node_id == node_id) {
                it = sessions_.erase(it);
                ++n;
            } else {
                ++it;
            }
        }
        return n;
    }

    std::size_t count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return sessions_.size();
    }

    std::size_t count_on_node(const std::string& node_id) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t n = 0;
        for (const auto& kv : sessions_) {
            if (kv.second.node_id == node_id) ++n;
        }
        return n;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, SessionLocation> sessions_;
};

}  // namespace cluster
}  // namespace chwell
