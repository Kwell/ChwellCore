#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "chwell/ops/analytics.h"
#include "chwell/ops/analytics_store.h"
#include "chwell/storage/memory_storage.h"

using namespace chwell;

TEST(AnalyticsStoreTest, FlushAndLoadCounts) {
    ops::AnalyticsPipeline ap;
    ap.track("login", "u1", 100);
    ap.track("login", "u2", 110);
    ap.track("login", "u1", 120);

    storage::MemoryStorage mem;
    ops::AnalyticsStore store(&ap, &mem);

    ASSERT_TRUE(store.flush_counts("login", 0, 1000));
    std::uint64_t c = 0, u = 0;
    ASSERT_TRUE(store.load_count("login", 0, 1000, c));
    ASSERT_TRUE(store.load_uv("login", 0, 1000, u));
    EXPECT_EQ(3u, c);
    EXPECT_EQ(2u, u);
}

TEST(AnalyticsStoreTest, FlushTopSnapshot) {
    ops::AnalyticsPipeline ap;
    ap.track("a", "u", 1);
    ap.track("a", "u", 2);
    ap.track("b", "u", 3);

    storage::MemoryStorage mem;
    ops::AnalyticsStore store(&ap, &mem);
    ASSERT_TRUE(store.flush_top("20260929", 5));

    std::string json;
    ASSERT_TRUE(store.load_top("20260929", json));
    EXPECT_NE(std::string::npos, json.find("\"a\""));
    EXPECT_NE(std::string::npos, json.find("\"count\":2"));
}

TEST(AnalyticsStoreTest, LoadMissingFails) {
    storage::MemoryStorage mem;
    ops::AnalyticsPipeline ap;
    ops::AnalyticsStore store(&ap, &mem);
    std::uint64_t v = 0;
    EXPECT_FALSE(store.load_count("nope", 0, 1, v));
    std::string json;
    EXPECT_FALSE(store.load_top("missing", json));
}

TEST(AnalyticsStoreTest, NullPipelineSafe) {
    storage::MemoryStorage mem;
    ops::AnalyticsStore store(nullptr, &mem);
    EXPECT_FALSE(store.flush_counts("x", 0, 1));
    EXPECT_FALSE(store.flush_top("s"));
}
