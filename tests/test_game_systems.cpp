#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "chwell/game/leaderboard.h"
#include "chwell/game/mail.h"
#include "chwell/game/wallet.h"

using namespace chwell::game;

// ==================== Leaderboard ====================

TEST(LeaderboardTest, UpdateAndTop) {
    Leaderboard lb;
    lb.update("alice", 100);
    lb.update("bob", 300);
    lb.update("carol", 200);

    auto top = lb.top(3);
    ASSERT_EQ(3u, top.size());
    EXPECT_EQ("bob", top[0].player_id);
    EXPECT_EQ(1, top[0].rank);
    EXPECT_EQ("carol", top[1].player_id);
    EXPECT_EQ("alice", top[2].player_id);
    EXPECT_EQ(3, top[2].rank);
}

TEST(LeaderboardTest, TopLimitsN) {
    Leaderboard lb;
    for (int i = 0; i < 10; ++i) {
        lb.update("p" + std::to_string(i), i * 10);
    }
    auto top = lb.top(3);
    ASSERT_EQ(3u, top.size());
    EXPECT_EQ("p9", top[0].player_id);  // 90
    EXPECT_EQ("p8", top[1].player_id);
    EXPECT_EQ("p7", top[2].player_id);
}

TEST(LeaderboardTest, UpdateOverwritesScore) {
    Leaderboard lb;
    lb.update("alice", 100);
    lb.update("alice", 50);
    std::int64_t score = 0;
    ASSERT_TRUE(lb.score_of("alice", score));
    EXPECT_EQ(50, score);
    EXPECT_EQ(1u, lb.count());
}

TEST(LeaderboardTest, RankOf) {
    Leaderboard lb;
    lb.update("a", 10);
    lb.update("b", 20);
    lb.update("c", 30);
    EXPECT_EQ(1, lb.rank_of("c"));
    EXPECT_EQ(2, lb.rank_of("b"));
    EXPECT_EQ(3, lb.rank_of("a"));
    EXPECT_EQ(0, lb.rank_of("missing"));
}

TEST(LeaderboardTest, RemoveAndClear) {
    Leaderboard lb;
    lb.update("a", 1);
    lb.update("b", 2);
    EXPECT_TRUE(lb.remove("a"));
    EXPECT_FALSE(lb.remove("a"));
    EXPECT_EQ(1u, lb.count());
    lb.clear();
    EXPECT_EQ(0u, lb.count());
}

TEST(LeaderboardTest, SameScoreTieBreakByTimeThenId) {
    Leaderboard lb;
    lb.update("a", 100, /*now=*/1);
    lb.update("b", 100, /*now=*/2);
    // 同分：update_time 新的在前
    auto top = lb.top(2);
    ASSERT_EQ(2u, top.size());
    EXPECT_EQ("b", top[0].player_id);
    EXPECT_EQ("a", top[1].player_id);

    Leaderboard lb2;
    lb2.update("a", 100, /*now=*/5);
    lb2.update("b", 100, /*now=*/5);
    auto top2 = lb2.top(2);
    ASSERT_EQ(2u, top2.size());
    EXPECT_EQ("a", top2[0].player_id);  // 同分同时间按 id 字典序
    EXPECT_EQ("b", top2[1].player_id);
}

// ==================== Mail ====================

TEST(MailboxTest, SendAndList) {
    Mailbox box;
    auto id = box.send("system", "alice", "hello", "world");
    EXPECT_GT(id, 0);

    auto mails = box.list("alice");
    ASSERT_EQ(1u, mails.size());
    EXPECT_EQ("hello", mails[0].title);
    EXPECT_EQ("system", mails[0].from);
    EXPECT_FALSE(mails[0].read);
}

TEST(MailboxTest, MarkReadAndFilter) {
    Mailbox box;
    box.send("sys", "bob", "t1", "b1");
    box.send("sys", "bob", "t2", "b2");

    auto unread = box.list("bob", /*include_read=*/false);
    EXPECT_EQ(2u, unread.size());
    EXPECT_EQ(2u, box.unread_count("bob"));

    ASSERT_TRUE(box.mark_read(unread[0].mail_id));
    auto unread2 = box.list("bob", /*include_read=*/false);
    EXPECT_EQ(1u, unread2.size());
    EXPECT_EQ(1u, box.unread_count("bob"));

    auto all = box.list("bob", /*include_read=*/true);
    EXPECT_EQ(2u, all.size());
}

TEST(MailboxTest, RemoveMail) {
    Mailbox box;
    auto id = box.send("sys", "c", "t", "b");
    EXPECT_EQ(1u, box.total());
    EXPECT_TRUE(box.remove(id));
    EXPECT_FALSE(box.remove(id));
    EXPECT_EQ(0u, box.total());
    EXPECT_TRUE(box.list("c").empty());
}

TEST(MailboxTest, ExpireDroppedFromList) {
    Mailbox box;
    auto id = box.send("sys", "d", "t", "b", {}, /*now=*/1000, /*ttl_seconds=*/1);
    // ttl_seconds=1 → expire_time = 1000 + 1000 = 2000（ms 语义见实现）
    Mail m;
    ASSERT_TRUE(box.get(id, m));
    EXPECT_EQ(2000, m.expire_time);

    auto before = box.list("d", true, /*now=*/1500);
    EXPECT_EQ(1u, before.size());
    auto after = box.list("d", true, /*now=*/2500);
    EXPECT_TRUE(after.empty());
}

TEST(MailboxTest, AttachmentsCarried) {
    Mailbox box;
    std::vector<MailAttachment> att = {{"gold", 100}, {"exp", 50}};
    auto id = box.send("sys", "e", "t", "b", std::move(att));
    Mail m;
    ASSERT_TRUE(box.get(id, m));
    ASSERT_EQ(2u, m.attachments.size());
    EXPECT_EQ("gold", m.attachments[0].item_id);
    EXPECT_EQ(100, m.attachments[0].count);
}

TEST(MailboxTest, SendToEmptyFails) {
    Mailbox box;
    EXPECT_EQ(0, box.send("sys", "", "t", "b"));
}

// ==================== Wallet ====================

TEST(WalletTest, AddAndSpend) {
    Wallet w;
    EXPECT_TRUE(w.add("p1", "gold", 100));
    EXPECT_EQ(100, w.available("p1", "gold"));
    EXPECT_TRUE(w.spend("p1", "gold", 30));
    EXPECT_EQ(70, w.available("p1", "gold"));
    EXPECT_FALSE(w.spend("p1", "gold", 1000));  // 不足
    EXPECT_EQ(70, w.available("p1", "gold"));
}

TEST(WalletTest, RejectsNonPositive) {
    Wallet w;
    EXPECT_FALSE(w.add("p1", "gold", 0));
    EXPECT_FALSE(w.add("p1", "gold", -5));
    EXPECT_FALSE(w.spend("p1", "gold", -1));
    EXPECT_FALSE(w.add("", "gold", 1));
    EXPECT_FALSE(w.add("p1", "", 1));
}

TEST(WalletTest, CurrenciesAreIndependent) {
    Wallet w;
    w.add("p1", "gold", 10);
    w.add("p1", "gem", 3);
    EXPECT_EQ(10, w.available("p1", "gold"));
    EXPECT_EQ(3, w.available("p1", "gem"));
    EXPECT_EQ(0, w.available("p1", "diamond"));
}

TEST(WalletTest, HoldConfirmConsumesFrozen) {
    Wallet w;
    w.add("p1", "gold", 100);

    ASSERT_TRUE(w.try_hold("h1", "p1", "gold", 40));
    auto b = w.balance("p1", "gold");
    EXPECT_EQ(60, b.available);
    EXPECT_EQ(40, b.frozen);
    EXPECT_EQ(100, b.total());

    ASSERT_TRUE(w.confirm_hold("h1"));
    b = w.balance("p1", "gold");
    EXPECT_EQ(60, b.available);
    EXPECT_EQ(0, b.frozen);
    EXPECT_EQ(60, b.total());
    EXPECT_FALSE(w.hold_exists("h1"));
}

TEST(WalletTest, HoldCancelRefunds) {
    Wallet w;
    w.add("p1", "gold", 100);
    ASSERT_TRUE(w.try_hold("h1", "p1", "gold", 40));
    ASSERT_TRUE(w.cancel_hold("h1"));
    auto b = w.balance("p1", "gold");
    EXPECT_EQ(100, b.available);
    EXPECT_EQ(0, b.frozen);
}

TEST(WalletTest, HoldInsufficientFails) {
    Wallet w;
    w.add("p1", "gold", 10);
    EXPECT_FALSE(w.try_hold("h1", "p1", "gold", 50));
    EXPECT_EQ(10, w.available("p1", "gold"));
    EXPECT_EQ(0u, w.hold_count());
}

TEST(WalletTest, DuplicateHoldIdRejected) {
    Wallet w;
    w.add("p1", "gold", 100);
    ASSERT_TRUE(w.try_hold("h1", "p1", "gold", 10));
    EXPECT_FALSE(w.try_hold("h1", "p1", "gold", 10));
    EXPECT_EQ(1u, w.hold_count());
}

TEST(WalletTest, ConfirmCancelUnknownHoldFail) {
    Wallet w;
    EXPECT_FALSE(w.confirm_hold("nope"));
    EXPECT_FALSE(w.cancel_hold("nope"));
}

TEST(WalletTest, HoldBlocksSpend) {
    Wallet w;
    w.add("p1", "gold", 100);
    ASSERT_TRUE(w.try_hold("h1", "p1", "gold", 90));
    // 可用仅 10
    EXPECT_FALSE(w.spend("p1", "gold", 20));
    EXPECT_TRUE(w.spend("p1", "gold", 10));
    ASSERT_TRUE(w.cancel_hold("h1"));
    EXPECT_EQ(90, w.available("p1", "gold"));
}
