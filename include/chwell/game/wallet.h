#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace chwell {
namespace game {

// ========== 账本 / 钱包 ==========
//
// 多币种余额账本，支持：
// - add / spend（余额不足失败）
// - TCC 风格预留：try_hold 冻结金额，confirm_hold 落账，cancel_hold 解冻
// - 查询余额 / 冻结额 / 可用额
//
// 纯内存实现，可再包 WriteBackCache 持久化。金额为 int64，不做溢出饱和
// （调用方保证加减合法）；spend/hold 一律先校验可用额。

class Wallet {
public:
    struct Balance {
        std::int64_t available = 0;
        std::int64_t frozen = 0;
        std::int64_t total() const { return available + frozen; }
    };

    // 充值 / 奖励
    bool add(const std::string& player_id, const std::string& currency, std::int64_t amount) {
        if (amount <= 0 || player_id.empty() || currency.empty()) return false;
        std::lock_guard<std::mutex> lock(mu_);
        Balance& b = balances_[key(player_id, currency)];
        b.available += amount;
        return true;
    }

    // 直接扣除；余额不足返回 false
    bool spend(const std::string& player_id, const std::string& currency, std::int64_t amount) {
        if (amount <= 0 || player_id.empty() || currency.empty()) return false;
        std::lock_guard<std::mutex> lock(mu_);
        Balance& b = balances_[key(player_id, currency)];
        if (b.available < amount) return false;
        b.available -= amount;
        return true;
    }

    // ===== TCC 预留 =====

    // Try：冻结 amount；可用不足返回 false（hold_id 需全局唯一）
    bool try_hold(const std::string& hold_id,
                  const std::string& player_id,
                  const std::string& currency,
                  std::int64_t amount) {
        if (hold_id.empty() || amount <= 0 || player_id.empty() || currency.empty()) return false;
        std::lock_guard<std::mutex> lock(mu_);
        if (holds_.count(hold_id)) return false;  // 幂等冲突
        Balance& b = balances_[key(player_id, currency)];
        if (b.available < amount) return false;
        b.available -= amount;
        b.frozen += amount;
        Hold h;
        h.player_id = player_id;
        h.currency = currency;
        h.amount = amount;
        holds_[hold_id] = h;
        return true;
    }

    // Confirm：冻结转扣除；hold 不存在返回 false
    bool confirm_hold(const std::string& hold_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = holds_.find(hold_id);
        if (it == holds_.end()) return false;
        Balance& b = balances_[key(it->second.player_id, it->second.currency)];
        b.frozen -= it->second.amount;
        // 冻结已从 available 扣掉，confirm 即消费完成
        holds_.erase(it);
        return true;
    }

    // Cancel：冻结退回可用
    bool cancel_hold(const std::string& hold_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = holds_.find(hold_id);
        if (it == holds_.end()) return false;
        Balance& b = balances_[key(it->second.player_id, it->second.currency)];
        b.frozen -= it->second.amount;
        b.available += it->second.amount;
        holds_.erase(it);
        return true;
    }

    bool hold_exists(const std::string& hold_id) const {
        std::lock_guard<std::mutex> lock(mu_);
        return holds_.count(hold_id) > 0;
    }

    Balance balance(const std::string& player_id, const std::string& currency) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = balances_.find(key(player_id, currency));
        if (it == balances_.end()) return Balance();
        return it->second;
    }

    std::int64_t available(const std::string& player_id, const std::string& currency) const {
        return balance(player_id, currency).available;
    }

    std::size_t hold_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return holds_.size();
    }

private:
    struct Hold {
        std::string player_id;
        std::string currency;
        std::int64_t amount = 0;
    };

    static std::string key(const std::string& player_id, const std::string& currency) {
        return player_id + '\x1f' + currency;
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, Balance> balances_;
    std::unordered_map<std::string, Hold> holds_;
};

}  // namespace game
}  // namespace chwell
