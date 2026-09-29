#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "chwell/game/replay.h"

using namespace chwell::game;

TEST(ReplayRecorderTest, AddAndRange) {
    ReplayRecorder rec;
    ReplayEvent e1; e1.time_ms = 100; e1.player_id = "a"; e1.cmd = 1;
    ReplayEvent e2; e2.time_ms = 200; e2.player_id = "b"; e2.cmd = 2;
    ReplayEvent e3; e3.time_ms = 300; e3.player_id = "a"; e3.cmd = 1;
    EXPECT_TRUE(rec.add(e1));
    EXPECT_TRUE(rec.add(e2));
    EXPECT_TRUE(rec.add(e3));
    EXPECT_EQ(3u, rec.size());

    auto mid = rec.range(150, 250);
    ASSERT_EQ(1u, mid.size());
    EXPECT_EQ("b", mid[0].player_id);
}

TEST(ReplayRecorderTest, RejectsTimeGoingBackwards) {
    ReplayRecorder rec;
    ReplayEvent e1; e1.time_ms = 200;
    ReplayEvent e2; e2.time_ms = 100;
    ASSERT_TRUE(rec.add(e1));
    EXPECT_FALSE(rec.add(e2));
    EXPECT_EQ(1u, rec.size());
}

TEST(ReplayRecorderTest, AllAndClear) {
    ReplayRecorder rec;
    ReplayEvent e; e.time_ms = 1;
    rec.add(e);
    EXPECT_EQ(1u, rec.all().size());
    rec.clear();
    EXPECT_EQ(0u, rec.size());
}

TEST(SpectatorFeedTest, PushAndLatest) {
    SpectatorFeed feed;
    EXPECT_EQ(1, feed.push({'x'}, 100));
    EXPECT_EQ(2, feed.push({'y'}, 200));

    SpectatorFeed::Frame f;
    ASSERT_TRUE(feed.latest(f));
    EXPECT_EQ(2, f.seq);
    EXPECT_EQ(200, f.time_ms);
    ASSERT_EQ(1u, f.data.size());
    EXPECT_EQ('y', f.data[0]);
}

TEST(SpectatorFeedTest, TakeAfterIncremental) {
    SpectatorFeed feed;
    feed.push({'a'}, 1);
    feed.push({'b'}, 2);
    feed.push({'c'}, 3);

    auto batch = feed.take_after(1);
    ASSERT_EQ(2u, batch.size());
    EXPECT_EQ(2, batch[0].seq);
    EXPECT_EQ(3, batch[1].seq);

    EXPECT_TRUE(feed.take_after(3).empty());
}

TEST(SpectatorFeedTest, MaxFramesEvictsOldest) {
    SpectatorFeed feed;
    feed.set_max_frames(3);
    for (int i = 0; i < 5; ++i) feed.push({static_cast<char>('0' + i)}, i);
    EXPECT_EQ(5, feed.seq());

    auto all = feed.take_after(0);
    EXPECT_EQ(3u, all.size());  // 只保留最近 3 帧
    EXPECT_EQ(3, all[0].seq);   // seq 1、2 被淘汰
    EXPECT_EQ(5, all[2].seq);
}

TEST(SpectatorFeedTest, LatestOnEmptyFails) {
    SpectatorFeed feed;
    SpectatorFeed::Frame f;
    EXPECT_FALSE(feed.latest(f));
}
