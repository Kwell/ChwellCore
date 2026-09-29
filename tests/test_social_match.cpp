#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "chwell/game/social_match.h"

using namespace chwell::game;

// ==================== SocialGraph ====================

TEST(SocialGraphTest, FollowIsOneWay) {
    SocialGraph g;
    g.follow("a", "b");
    EXPECT_TRUE(g.is_following("a", "b"));
    EXPECT_FALSE(g.is_following("b", "a"));
    EXPECT_FALSE(g.is_friend("a", "b"));
}

TEST(SocialGraphTest, MutualFollowBecomesFriend) {
    SocialGraph g;
    g.add_friend("a", "b");
    EXPECT_TRUE(g.is_friend("a", "b"));
    EXPECT_TRUE(g.is_friend("b", "a"));
    auto friends = g.friends_of("a");
    ASSERT_EQ(1u, friends.size());
    EXPECT_EQ("b", friends[0]);
}

TEST(SocialGraphTest, RemoveFriendBreaksBoth) {
    SocialGraph g;
    g.add_friend("a", "b");
    g.remove_friend("a", "b");
    EXPECT_FALSE(g.is_friend("a", "b"));
    EXPECT_FALSE(g.is_following("a", "b"));
    EXPECT_TRUE(g.friends_of("a").empty());
}

TEST(SocialGraphTest, FollowersListed) {
    SocialGraph g;
    g.follow("x", "target");
    g.follow("y", "target");
    auto fl = g.followers_of("target");
    EXPECT_EQ(2u, fl.size());
}

TEST(SocialGraphTest, Blocklist) {
    SocialGraph g;
    g.block("a", "b");
    EXPECT_TRUE(g.is_blocked("a", "b"));
    EXPECT_FALSE(g.is_blocked("b", "a"));
    g.unblock("a", "b");
    EXPECT_FALSE(g.is_blocked("a", "b"));
}

TEST(SocialGraphTest, SelfAndEmptyIgnored) {
    SocialGraph g;
    g.follow("a", "a");
    g.follow("", "b");
    g.follow("a", "");
    EXPECT_FALSE(g.is_following("a", "a"));
}

// ==================== Matchmaker ====================

TEST(MatchmakerTest, MatchesWhenTeamFull) {
    Matchmaker mm(2, 100);
    EXPECT_TRUE(mm.enqueue("p1", 50));
    EXPECT_TRUE(mm.try_match().empty());
    EXPECT_TRUE(mm.enqueue("p2", 60));
    auto team = mm.try_match();
    ASSERT_EQ(2u, team.size());
    EXPECT_EQ("p1", team[0]);  // FIFO
    EXPECT_EQ("p2", team[1]);
    EXPECT_EQ(0u, mm.total_waiting());
}

TEST(MatchmakerTest, SameBucketPreferred) {
    Matchmaker mm(2, 100);
    mm.enqueue("low1", 10);
    mm.enqueue("low2", 20);
    mm.enqueue("high1", 500);
    auto team = mm.try_match();
    ASSERT_EQ(2u, team.size());
    EXPECT_EQ("low1", team[0]);
    EXPECT_EQ("low2", team[1]);
    EXPECT_EQ(1u, mm.total_waiting());  // high1 还在等
}

TEST(MatchmakerTest, DuplicateEnqueueRejected) {
    Matchmaker mm(2, 100);
    EXPECT_TRUE(mm.enqueue("p1", 1));
    EXPECT_FALSE(mm.enqueue("p1", 2));
    EXPECT_EQ(1u, mm.total_waiting());
}

TEST(MatchmakerTest, CancelRemovesFromQueue) {
    Matchmaker mm(2, 100);
    mm.enqueue("p1", 1);
    EXPECT_TRUE(mm.cancel("p1"));
    EXPECT_FALSE(mm.cancel("p1"));
    EXPECT_EQ(0u, mm.total_waiting());
}

TEST(MatchmakerTest, TeamSizeThree) {
    Matchmaker mm(3, 100);
    mm.enqueue("a", 1);
    mm.enqueue("b", 1);
    EXPECT_TRUE(mm.try_match().empty());
    mm.enqueue("c", 1);
    auto team = mm.try_match();
    EXPECT_EQ(3u, team.size());
}

TEST(MatchmakerTest, QueueSizeByBucket) {
    Matchmaker mm(2, 100);
    mm.enqueue("a", 50);
    mm.enqueue("b", 55);
    mm.enqueue("c", 250);
    EXPECT_EQ(2u, mm.queue_size(50));
    EXPECT_EQ(1u, mm.queue_size(250));
    EXPECT_EQ(0u, mm.queue_size(9999));
}
