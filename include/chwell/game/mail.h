#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace chwell {
namespace game {

// ========== 邮件 ==========
//
// 内存邮箱：按收件人分桶，支持发送 / 拉取 / 已读 / 删除。
// 状态机：unread → read → deleted（deleted 立即从列表移除）。
// 附件为纯字符串载荷（业务侧自行解析），本模块不做扣发放。

struct MailAttachment {
    std::string item_id;
    int count = 0;
};

struct Mail {
    std::int64_t mail_id = 0;
    std::string from;
    std::string to;
    std::string title;
    std::string body;
    std::vector<MailAttachment> attachments;
    std::int64_t send_time = 0;
    std::int64_t expire_time = 0;  // 0 表示不过期
    bool read = false;
};

class Mailbox {
public:
    // 发送；返回 mail_id（>0 成功）
    std::int64_t send(const std::string& from,
                      const std::string& to,
                      const std::string& title,
                      const std::string& body,
                      std::vector<MailAttachment> attachments = {},
                      std::int64_t now = 0,
                      std::int64_t ttl_seconds = 0) {
        if (to.empty()) return 0;
        std::lock_guard<std::mutex> lock(mu_);
        Mail m;
        m.mail_id = ++next_id_;
        m.from = from;
        m.to = to;
        m.title = title;
        m.body = body;
        m.attachments = std::move(attachments);
        m.send_time = now;
        m.expire_time = ttl_seconds > 0 ? now + ttl_seconds * 1000 : 0;
        mail_[m.mail_id] = m;
        inbox_[to].push_back(m.mail_id);
        return m.mail_id;
    }

    // 拉取收件箱（自动丢弃过期）；include_read=false 仅未读
    std::vector<Mail> list(const std::string& to, bool include_read = true, std::int64_t now = 0) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Mail> out;
        auto it = inbox_.find(to);
        if (it == inbox_.end()) return out;
        for (std::int64_t id : it->second) {
            auto mi = mail_.find(id);
            if (mi == mail_.end()) continue;
            const Mail& m = mi->second;
            if (m.expire_time > 0 && now > m.expire_time) continue;
            if (!include_read && m.read) continue;
            out.push_back(m);
        }
        return out;
    }

    // 取单封
    bool get(std::int64_t mail_id, Mail& out) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = mail_.find(mail_id);
        if (it == mail_.end()) return false;
        out = it->second;
        return true;
    }

    // 标记已读；返回是否找到
    bool mark_read(std::int64_t mail_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = mail_.find(mail_id);
        if (it == mail_.end()) return false;
        it->second.read = true;
        return true;
    }

    // 删除；返回是否找到
    bool remove(std::int64_t mail_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = mail_.find(mail_id);
        if (it == mail_.end()) return false;
        const std::string to = it->second.to;
        mail_.erase(it);
        auto inbox = inbox_.find(to);
        if (inbox != inbox_.end()) {
            auto& ids = inbox->second;
            for (auto i = ids.begin(); i != ids.end(); ++i) {
                if (*i == mail_id) {
                    ids.erase(i);
                    break;
                }
            }
        }
        return true;
    }

    std::size_t unread_count(const std::string& to) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t n = 0;
        auto it = inbox_.find(to);
        if (it == inbox_.end()) return 0;
        for (std::int64_t id : it->second) {
            auto mi = mail_.find(id);
            if (mi != mail_.end() && !mi->second.read) ++n;
        }
        return n;
    }

    std::size_t total() const {
        std::lock_guard<std::mutex> lock(mu_);
        return mail_.size();
    }

private:
    mutable std::mutex mu_;
    std::int64_t next_id_ = 0;
    std::unordered_map<std::int64_t, Mail> mail_;
    std::unordered_map<std::string, std::vector<std::int64_t>> inbox_;
};

}  // namespace game
}  // namespace chwell
