#pragma once

// Saga 编排事务：多步骤 + 失败逆序补偿
//
// 适用：长事务、跨服务流程，如「扣金币 → 发道具 → 记日志」
//   step1: act=扣金币  comp=退金币
//   step2: act=发道具  comp=回收道具
//   step3: act=记日志  comp=删日志
// 任一步失败 → 对已完成步骤逆序执行 comp，保证最终一致。
//
// 用法：
//   transaction::Saga saga("order-1");
//   saga.add_step("deduct_gold",  [&]{ return gold.deduct(); },
//                                [&]{ return gold.refund(); });
//   saga.add_step("grant_item",   [&]{ return item.grant(); },
//                                [&]{ return item.revoke(); });
//   bool ok = saga.run();
//
// 语义：
// - 只补偿「已成功完成」的步骤；失败步骤自身需在 act 内回滚局部效果
//   （可选 compensate_failed_=true 时也补偿失败步骤）
// - 补偿本身失败会记录，但继续补偿其余步骤（尽力而为）
// - 全程写入 log()，便于审计与运维排查

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <cstdint>

#include "chwell/core/logger.h"

namespace chwell {
namespace transaction {

enum class SagaState {
    Init,
    Running,
    Compensating,
    Completed,
    Compensated,   // 失败并已补偿
    PartialFail    // 失败且补偿也有失败（需人工介入）
};

inline const char* saga_state_name(SagaState s) {
    switch (s) {
        case SagaState::Init: return "Init";
        case SagaState::Running: return "Running";
        case SagaState::Compensating: return "Compensating";
        case SagaState::Completed: return "Completed";
        case SagaState::Compensated: return "Compensated";
        case SagaState::PartialFail: return "PartialFail";
    }
    return "?";
}

class Saga {
public:
    struct Step {
        std::string name;
        std::function<bool()> act;
        std::function<bool()> compensate;
    };

    explicit Saga(std::string id) : id_(std::move(id)) {}

    void add_step(std::string name, std::function<bool()> act, std::function<bool()> compensate) {
        std::lock_guard<std::mutex> lock(mutex_);
        steps_.push_back(Step{std::move(name), std::move(act), std::move(compensate)});
    }

    // 失败步骤是否也参与补偿（默认 false：act 应自清理局部效果）
    void set_compensate_failed(bool v) {
        std::lock_guard<std::mutex> lock(mutex_);
        compensate_failed_ = v;
    }

    // 执行全部步骤；失败则逆序补偿
    bool run() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != SagaState::Init) {
            CHWELL_LOG_WARN("Saga " + id_ + ": run() called in state " + saga_state_name(state_));
            return state_ == SagaState::Completed;
        }
        state_ = SagaState::Running;

        std::vector<std::size_t> completed;
        for (std::size_t i = 0; i < steps_.size(); ++i) {
            auto& s = steps_[i];
            log_.push_back("run:" + s.name);
            bool ok = false;
            try {
                ok = s.act ? s.act() : true;
            } catch (const std::exception& e) {
                CHWELL_LOG_ERROR("Saga " + id_ + ": step " + s.name + " threw: " + e.what());
                ok = false;
            }
            if (!ok) {
                failed_step_ = s.name;
                CHWELL_LOG_WARN("Saga " + id_ + ": step " + s.name + " failed, compensating " +
                                std::to_string(completed.size()) + " completed steps");
                if (compensate_failed_) {
                    completed.push_back(i);
                }
                compensate_range(completed);
                state_ = any_comp_fail_ ? SagaState::PartialFail : SagaState::Compensated;
                return false;
            }
            completed.push_back(i);
        }
        state_ = SagaState::Completed;
        return true;
    }

    SagaState state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }
    const std::string& id() const { return id_; }
    std::string failed_step() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_step_;
    }
    std::vector<std::string> log() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return log_;
    }
    std::vector<std::string> compensation_failures() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return comp_failed_;
    }

private:
    void compensate_range(const std::vector<std::size_t>& idxs) {
        state_ = SagaState::Compensating;
        for (auto it = idxs.rbegin(); it != idxs.rend(); ++it) {
            auto& s = steps_[*it];
            log_.push_back("comp:" + s.name);
            bool ok = false;
            try {
                ok = s.compensate ? s.compensate() : true;
            } catch (const std::exception& e) {
                CHWELL_LOG_ERROR("Saga " + id_ + ": compensate " + s.name + " threw: " + e.what());
                ok = false;
            }
            if (!ok) {
                comp_failed_.push_back(s.name);
                any_comp_fail_ = true;
                CHWELL_LOG_ERROR("Saga " + id_ + ": compensate failed for " + s.name +
                                 " (manual intervention required)");
            }
        }
    }

    std::string id_;
    std::vector<Step> steps_;
    std::vector<std::string> log_;
    std::string failed_step_;
    std::vector<std::string> comp_failed_;
    SagaState state_ = SagaState::Init;
    bool compensate_failed_ = false;
    bool any_comp_fail_ = false;
    mutable std::mutex mutex_;
};

} // namespace transaction
} // namespace chwell
