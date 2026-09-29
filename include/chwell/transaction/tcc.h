#pragma once

// TCC（Try-Confirm-Cancel）两阶段补偿事务
//
// 适用：跨服务的原子操作，如「扣金币 + 发道具 + 记日志」
//   1. Try     ：各参与者预留资源（冻结金币、占道具槽）
//   2. Confirm ：全部 Try 成功后提交（真正扣减）
//   3. Cancel  ：任一 Try 失败，对已 Try 的参与者回滚（解冻）
//
// 用法：
//   tcc::TccTransaction tx("tx-123");
//   tx.add_participant(&gold);     // ITccParticipant*
//   tx.add_participant(&item);
//   if (tx.commit()) { /* 已确认 */ }
//   // tx.state() == Confirmed / Cancelled
//
// 语义：
// - Try 阶段失败 → 对所有已 Try 的参与者（含失败者）调用 Cancel
// - Confirm 阶段失败 → 不自动回滚（资源已提交），需上层重试 Confirm
//   （生产建议给 Confirm 加幂等 + 重试队列）

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

#include "chwell/core/logger.h"

namespace chwell {
namespace transaction {

enum class TccState {
    Init,
    Trying,
    Confirmed,
    Cancelled
};

inline const char* tcc_state_name(TccState s) {
    switch (s) {
        case TccState::Init: return "Init";
        case TccState::Trying: return "Trying";
        case TccState::Confirmed: return "Confirmed";
        case TccState::Cancelled: return "Cancelled";
    }
    return "?";
}

// TCC 参与者接口：业务侧实现三种操作，均需幂等
class ITccParticipant {
public:
    virtual ~ITccParticipant() = default;

    virtual std::string name() const = 0;

    // 预留资源（冻结/占位）。成功返回 true；失败必须自行回滚已产生的部分效果
    virtual bool try_reserve(const std::string& tx_id) = 0;

    // 提交（真正扣减/发放）。必须幂等
    virtual bool confirm(const std::string& tx_id) = 0;

    // 回滚预留。必须幂等
    virtual bool cancel(const std::string& tx_id) = 0;
};

class TccTransaction {
public:
    explicit TccTransaction(std::string tx_id)
        : tx_id_(std::move(tx_id)) {}

    // 非拥有指针：调用方保证生命周期覆盖事务
    void add_participant(ITccParticipant* p) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (p) participants_.push_back(p);
    }

    // 两阶段提交：Try 全部 → Confirm 全部；任一 Try 失败则 Cancel 全部已试
    bool commit() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != TccState::Init) {
            CHWELL_LOG_WARN("TCC " + tx_id_ + ": commit() called in state " + tcc_state_name(state_));
            return state_ == TccState::Confirmed;
        }
        if (participants_.empty()) {
            state_ = TccState::Confirmed;
            return true;
        }

        state_ = TccState::Trying;
        std::vector<ITccParticipant*> tried;
        for (auto* p : participants_) {
            tried.push_back(p);
            if (!p->try_reserve(tx_id_)) {
                failed_ = p->name();
                CHWELL_LOG_WARN("TCC " + tx_id_ + ": try failed at " + p->name() + ", cancelling " +
                                std::to_string(tried.size()) + " participants");
                // 逆序 cancel 所有已试参与者（含失败者，因其 try 可能有部分效果）
                cancel_list(tried);
                state_ = TccState::Cancelled;
                return false;
            }
        }

        // 全部 try 成功，进入 confirm
        for (auto* p : participants_) {
            if (!p->confirm(tx_id_)) {
                // confirm 失败不回滚：资源已提交，需上层重试
                confirm_failed_ = p->name();
                CHWELL_LOG_ERROR("TCC " + tx_id_ + ": confirm failed at " + p->name() +
                                 " (manual retry required)");
                return false;
            }
        }
        state_ = TccState::Confirmed;
        return true;
    }

    // 显式取消（事务已创建但未 commit 时）
    bool cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == TccState::Cancelled) return true;
        if (state_ == TccState::Confirmed) return false;
        cancel_list(participants_);
        state_ = TccState::Cancelled;
        return true;
    }

    TccState state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }
    const std::string& id() const { return tx_id_; }
    std::string failed_participant() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_;
    }
    std::string confirm_failed_participant() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return confirm_failed_;
    }

private:
    void cancel_list(const std::vector<ITccParticipant*>& list) {
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            if (!(*it)->cancel(tx_id_)) {
                CHWELL_LOG_ERROR("TCC " + tx_id_ + ": cancel failed at " + (*it)->name() +
                                 " (resource leak risk)");
                cancel_failed_ = (*it)->name();
            }
        }
    }

    std::string tx_id_;
    std::vector<ITccParticipant*> participants_;
    TccState state_ = TccState::Init;
    std::string failed_;
    std::string confirm_failed_;
    std::string cancel_failed_;
    mutable std::mutex mutex_;
};

} // namespace transaction
} // namespace chwell
