#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace chwell {
namespace ops {

// ========== GM 后台指令中枢 ==========
//
// GmConsole：注册带权限等级的 GM 指令，执行前鉴权，执行后写审计日志。
// - register_command(name, min_level, handler)
// - invoke(actor, name, args) → 结果串 + 是否成功
// - 审计：who / when / cmd / args / ok / result
// - set_actor_level / actor_level
//
// 与 service/ops_component 的差别：这里是进程内指令表与审计，不做网络协议。
// 网络侧可在 OpsComponent 里把 0x03xx 报文翻译成 GmConsole::invoke。

struct GmAuditRecord {
    std::int64_t time_ms = 0;
    std::string actor;
    std::string command;
    std::string args;
    bool ok = false;
    std::string result;
};

class GmConsole {
public:
    using Handler = std::function<bool(const std::string& actor,
                                       const std::string& args,
                                       std::string& result)>;

    // min_level：低于该等级拒绝；0 为普通 GM
    void register_command(const std::string& name, int min_level, Handler handler) {
        if (name.empty() || !handler) return;
        std::lock_guard<std::mutex> lock(mu_);
        Command c;
        c.min_level = min_level;
        c.handler = std::move(handler);
        commands_[name] = std::move(c);
    }

    bool unregister_command(const std::string& name) {
        std::lock_guard<std::mutex> lock(mu_);
        return commands_.erase(name) > 0;
    }

    void set_actor_level(const std::string& actor, int level) {
        std::lock_guard<std::mutex> lock(mu_);
        actor_levels_[actor] = level;
    }

    int actor_level(const std::string& actor) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = actor_levels_.find(actor);
        return it == actor_levels_.end() ? -1 : it->second;
    }

    // 执行指令；成功返回 true，result 为输出
    bool invoke(const std::string& actor, const std::string& name,
                const std::string& args, std::string& result,
                std::int64_t now_ms = 0) {
        Handler handler;
        int min_level = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = commands_.find(name);
            if (it == commands_.end()) {
                result = "unknown command";
                append_audit_locked(now_ms, actor, name, args, false, result);
                return false;
            }
            handler = it->second.handler;
            min_level = it->second.min_level;

            auto ait = actor_levels_.find(actor);
            int level = ait == actor_levels_.end() ? -1 : ait->second;
            if (level < min_level) {
                result = "permission denied";
                append_audit_locked(now_ms, actor, name, args, false, result);
                return false;
            }
        }

        bool ok = false;
        try {
            ok = handler(actor, args, result);
        } catch (...) {
            ok = false;
            result = "handler exception";
        }
        std::lock_guard<std::mutex> lock(mu_);
        append_audit_locked(now_ms, actor, name, args, ok, result);
        return ok;
    }

    std::vector<GmAuditRecord> audit_log(std::size_t max = 100) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<GmAuditRecord> out;
        std::size_t n = audit_.size() > max ? audit_.size() - max : 0;
        for (std::size_t i = n; i < audit_.size(); ++i) out.push_back(audit_[i]);
        return out;
    }

    std::size_t command_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return commands_.size();
    }

    std::vector<std::string> command_names() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::string> out;
        out.reserve(commands_.size());
        for (const auto& kv : commands_) out.push_back(kv.first);
        std::sort(out.begin(), out.end());
        return out;
    }

    bool has_command(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mu_);
        return commands_.count(name) > 0;
    }

    void clear_audit() {
        std::lock_guard<std::mutex> lock(mu_);
        audit_.clear();
    }

private:
    struct Command {
        int min_level = 0;
        Handler handler;
    };

    void append_audit_locked(std::int64_t t, const std::string& actor,
                             const std::string& name, const std::string& args,
                             bool ok, const std::string& result) {
        GmAuditRecord r;
        r.time_ms = t;
        r.actor = actor;
        r.command = name;
        r.args = args;
        r.ok = ok;
        r.result = result;
        audit_.push_back(std::move(r));
        if (audit_.size() > 1000) {
            audit_.erase(audit_.begin(), audit_.begin() + (audit_.size() - 1000));
        }
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, Command> commands_;
    std::unordered_map<std::string, int> actor_levels_;
    std::vector<GmAuditRecord> audit_;
};

}  // namespace ops
}  // namespace chwell
