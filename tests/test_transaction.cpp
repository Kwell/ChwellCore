#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "chwell/transaction/tcc.h"
#include "chwell/transaction/saga.h"

using namespace chwell;
using namespace chwell::transaction;

namespace {

// ---- 测试用 TCC 参与者 ----
class MockGold : public ITccParticipant {
public:
    explicit MockGold(bool try_ok = true, bool confirm_ok = true)
        : try_ok_(try_ok), confirm_ok_(confirm_ok) {}

    std::string name() const override { return "gold"; }
    bool try_reserve(const std::string&) override {
        ++tries;
        if (!try_ok_) return false;
        frozen = true;
        return true;
    }
    bool confirm(const std::string&) override {
        ++confirms;
        if (!confirm_ok_) return false;
        frozen = false;
        committed = true;
        return true;
    }
    bool cancel(const std::string&) override {
        ++cancels;
        frozen = false;
        return true;
    }

    int tries = 0, confirms = 0, cancels = 0;
    bool frozen = false;
    bool committed = false;
    bool try_ok_;
    bool confirm_ok_;
};

class MockItem : public ITccParticipant {
public:
    std::string name() const override { return "item"; }
    bool try_reserve(const std::string&) override {
        ++tries;
        reserved = true;
        return true;
    }
    bool confirm(const std::string&) override {
        ++confirms;
        reserved = false;
        granted = true;
        return true;
    }
    bool cancel(const std::string&) override {
        ++cancels;
        reserved = false;
        return true;
    }
    int tries = 0, confirms = 0, cancels = 0;
    bool reserved = false;
    bool granted = false;
};

TEST(TccTest, AllTrySucceedConfirmAll) {
    MockGold gold;
    MockItem item;
    TccTransaction tx("t1");
    tx.add_participant(&gold);
    tx.add_participant(&item);

    EXPECT_TRUE(tx.commit());
    EXPECT_EQ(TccState::Confirmed, tx.state());
    EXPECT_TRUE(gold.committed);
    EXPECT_TRUE(item.granted);
    EXPECT_EQ(0, gold.cancels);
    EXPECT_EQ(0, item.cancels);
}

TEST(TccTest, TryFailureCancelsAllAttempted) {
    MockGold gold(/*try_ok=*/false);
    MockItem item;
    TccTransaction tx("t2");
    tx.add_participant(&gold);
    tx.add_participant(&item);

    EXPECT_FALSE(tx.commit());
    EXPECT_EQ(TccState::Cancelled, tx.state());
    EXPECT_EQ("gold", tx.failed_participant());
    // gold 尝试失败但也要 cancel（try 可能部分生效）
    EXPECT_GE(gold.cancels, 1);
    // item 未 try，不应 cancel
    EXPECT_EQ(0, item.cancels);
    EXPECT_FALSE(item.granted);
}

TEST(TccTest, SecondParticipantTryFailsCancelsFirst) {
    MockGold gold;
    MockItem item;
    // 让 item 的 try 失败
    struct BadItem : public ITccParticipant {
        std::string name() const override { return "bad_item"; }
        bool try_reserve(const std::string&) override { return false; }
        bool confirm(const std::string&) override { return true; }
        bool cancel(const std::string&) override { ++cancels; return true; }
        int cancels = 0;
    } bad;

    TccTransaction tx("t3");
    tx.add_participant(&gold);
    tx.add_participant(&bad);

    EXPECT_FALSE(tx.commit());
    EXPECT_EQ(TccState::Cancelled, tx.state());
    EXPECT_GE(gold.cancels, 1);   // 已 try 的 gold 被回滚
    EXPECT_GE(bad.cancels, 1);    // 失败者也被 cancel
    EXPECT_FALSE(gold.committed);
}

TEST(TccTest, ConfirmFailureDoesNotCancel) {
    MockGold gold(/*try_ok=*/true, /*confirm_ok=*/false);
    MockItem item;
    TccTransaction tx("t4");
    tx.add_participant(&gold);
    tx.add_participant(&item);

    EXPECT_FALSE(tx.commit());
    // confirm 失败不回滚
    EXPECT_EQ(0, gold.cancels);
    EXPECT_EQ("gold", tx.confirm_failed_participant());
}

TEST(TccTest, DoubleCommitIsNoOp) {
    MockGold gold;
    TccTransaction tx("t5");
    tx.add_participant(&gold);
    EXPECT_TRUE(tx.commit());
    EXPECT_TRUE(tx.commit());  // 幂等
    EXPECT_EQ(1, gold.confirms);
}

TEST(TccTest, EmptyTransactionCommits) {
    TccTransaction tx("t6");
    EXPECT_TRUE(tx.commit());
    EXPECT_EQ(TccState::Confirmed, tx.state());
}

TEST(TccTest, CancelBeforeCommit) {
    MockGold gold;
    TccTransaction tx("t7");
    tx.add_participant(&gold);
    EXPECT_TRUE(tx.cancel());
    EXPECT_EQ(TccState::Cancelled, tx.state());
    EXPECT_GE(gold.cancels, 1);
}

// ---- Saga ----

TEST(SagaTest, AllStepsSucceed) {
    std::vector<std::string> order;
    Saga saga("s1");
    saga.add_step("a", [&]{ order.push_back("a"); return true; },
                     [&]{ order.push_back("undo-a"); return true; });
    saga.add_step("b", [&]{ order.push_back("b"); return true; },
                     [&]{ order.push_back("undo-b"); return true; });

    EXPECT_TRUE(saga.run());
    EXPECT_EQ(SagaState::Completed, saga.state());
    ASSERT_EQ(2u, order.size());
    EXPECT_EQ("a", order[0]);
    EXPECT_EQ("b", order[1]);
}

TEST(SagaTest, FailureCompensatesInReverse) {
    std::vector<std::string> order;
    Saga saga("s2");
    saga.add_step("a", [&]{ order.push_back("a"); return true; },
                     [&]{ order.push_back("undo-a"); return true; });
    saga.add_step("b", [&]{ order.push_back("b"); return true; },
                     [&]{ order.push_back("undo-b"); return true; });
    saga.add_step("c", [&]{ return false; },  // failing step does not record success
                     [&]{ order.push_back("undo-c"); return true; });

    EXPECT_FALSE(saga.run());
    EXPECT_EQ(SagaState::Compensated, saga.state());
    EXPECT_EQ("c", saga.failed_step());
    // 已完成 a,b 被逆序补偿
    ASSERT_EQ(4u, order.size());
    EXPECT_EQ("a", order[0]);
    EXPECT_EQ("b", order[1]);
    EXPECT_EQ("undo-b", order[2]);
    EXPECT_EQ("undo-a", order[3]);
}

TEST(SagaTest, FirstStepFailsNoCompensation) {
    std::vector<std::string> order;
    Saga saga("s3");
    saga.add_step("a", [&]{ order.push_back("a"); return false; },
                     [&]{ order.push_back("undo-a"); return true; });

    EXPECT_FALSE(saga.run());
    EXPECT_EQ(SagaState::Compensated, saga.state());
    // 没有已完成步骤，不应有补偿
    ASSERT_EQ(1u, order.size());
    EXPECT_EQ("a", order[0]);
}

TEST(SagaTest, CompensateFailedStepWhenEnabled) {
    std::vector<std::string> order;
    Saga saga("s4");
    saga.set_compensate_failed(true);
    saga.add_step("a", [&]{ order.push_back("a"); return true; },
                     [&]{ order.push_back("undo-a"); return true; });
    saga.add_step("b", [&]{ order.push_back("b"); return false; },
                     [&]{ order.push_back("undo-b"); return true; });

    EXPECT_FALSE(saga.run());
    ASSERT_EQ(4u, order.size());
    EXPECT_EQ("undo-b", order[2]);
    EXPECT_EQ("undo-a", order[3]);
}

TEST(SagaTest, CompensationFailureMarkedPartialFail) {
    Saga saga("s5");
    saga.add_step("a", []{ return true; }, []{ return false; });  // 补偿失败
    saga.add_step("b", []{ return false; }, []{ return true; });

    EXPECT_FALSE(saga.run());
    EXPECT_EQ(SagaState::PartialFail, saga.state());
    ASSERT_EQ(1u, saga.compensation_failures().size());
    EXPECT_EQ("a", saga.compensation_failures()[0]);
}

TEST(SagaTest, StepExceptionIsFailure) {
    Saga saga("s6");
    saga.add_step("a", []{ return true; }, []{ return true; });
    saga.add_step("boom", []{ throw std::runtime_error("x"); return true; }, []{ return true; });

    EXPECT_FALSE(saga.run());
    EXPECT_EQ("boom", saga.failed_step());
    EXPECT_EQ(SagaState::Compensated, saga.state());
}

TEST(SagaTest, LogRecordsRunAndCompensate) {
    Saga saga("s7");
    saga.add_step("a", []{ return true; }, []{ return true; });
    saga.add_step("b", []{ return false; }, []{ return true; });
    saga.run();

    auto log = saga.log();
    ASSERT_GE(log.size(), 3u);
    EXPECT_EQ("run:a", log[0]);
    EXPECT_EQ("run:b", log[1]);
    EXPECT_EQ("comp:a", log[2]);
}

TEST(SagaTest, DoubleRunIsNoOp) {
    int ran = 0;
    Saga saga("s8");
    saga.add_step("a", [&]{ ++ran; return true; }, []{ return true; });
    EXPECT_TRUE(saga.run());
    EXPECT_TRUE(saga.run());  // 幂等
    EXPECT_EQ(1, ran);
}

// ---- TCC + Saga 组合：扣金币 + 发道具 ----

TEST(TransactionTest, GameSpendFlowTcc) {
    MockGold gold;
    MockItem item;
    TccTransaction tx("game-spend-1");
    tx.add_participant(&gold);
    tx.add_participant(&item);
    ASSERT_TRUE(tx.commit());
    EXPECT_TRUE(gold.committed);
    EXPECT_TRUE(item.granted);
}

TEST(TransactionTest, GameSpendFlowSaga) {
    int gold = 100, items = 0;
    Saga saga("game-spend-2");
    saga.add_step("deduct_gold",
        [&]{ if (gold < 10) return false; gold -= 10; return true; },
        [&]{ gold += 10; return true; });
    saga.add_step("grant_item",
        [&]{ items += 1; return true; },
        [&]{ items -= 1; return true; });

    ASSERT_TRUE(saga.run());
    EXPECT_EQ(90, gold);
    EXPECT_EQ(1, items);
}

TEST(TransactionTest, GameSpendFlowSagaRollback) {
    int gold = 100, items = 0;
    Saga saga("game-spend-3");
    saga.add_step("deduct_gold",
        [&]{ gold -= 10; return true; },
        [&]{ gold += 10; return true; });
    saga.add_step("grant_item",
        [&]{ return false; },  // 发放失败
        [&]{ items -= 1; return true; });

    EXPECT_FALSE(saga.run());
    // 金币已退回
    EXPECT_EQ(100, gold);
    EXPECT_EQ(0, items);
}

}  // namespace
