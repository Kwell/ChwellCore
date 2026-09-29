#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "chwell/ops/gray_release.h"

using namespace chwell::ops;

TEST(TrafficSplitterTest, SingleVersionTakesAll) {
    TrafficSplitter ts;
    ts.add_version("stable", 100);
    EXPECT_EQ("stable", ts.select("u1"));
    EXPECT_EQ("stable", ts.select_random(0.5));
}

TEST(TrafficSplitterTest, EmptyReturnsEmpty) {
    TrafficSplitter ts;
    EXPECT_EQ("", ts.select("u1"));
    EXPECT_EQ("", ts.select_random(0.5));
}

TEST(TrafficSplitterTest, SameKeyStable) {
    TrafficSplitter ts;
    ts.add_version("stable", 80);
    ts.add_version("canary", 20);
    std::string first = ts.select("user-42");
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(first, ts.select("user-42"));
    }
}

TEST(TrafficSplitterTest, ShareRatioApproxWeight) {
    TrafficSplitter ts;
    ts.add_version("stable", 90);
    ts.add_version("canary", 10);
    auto vs = ts.versions();
    ASSERT_EQ(2u, vs.size());
    double stable_share = 0, canary_share = 0;
    for (const auto& v : vs) {
        if (v.version == "stable") stable_share = v.share;
        if (v.version == "canary") canary_share = v.share;
    }
    EXPECT_NEAR(0.9, stable_share, 1e-6);
    EXPECT_NEAR(0.1, canary_share, 1e-6);
}

TEST(TrafficSplitterTest, SelectRandomRespectsWeights) {
    TrafficSplitter ts;
    ts.add_version("stable", 50);
    ts.add_version("canary", 50);
    EXPECT_EQ("canary", ts.select_random(0.1));   // 0.1 < 0.5 → canary（名字序：canary < stable）
    EXPECT_EQ("stable", ts.select_random(0.6));   // 0.6 >= 0.5 → stable
}

TEST(TrafficSplitterTest, ZeroWeightGetsNoTraffic) {
    TrafficSplitter ts;
    ts.add_version("off", 0);
    ts.add_version("on", 1);
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ("on", ts.select("k" + std::to_string(i)));
    }
}

TEST(TrafficSplitterTest, RemoveVersion) {
    TrafficSplitter ts;
    ts.add_version("a", 1);
    ts.add_version("b", 1);
    EXPECT_TRUE(ts.remove_version("a"));
    EXPECT_FALSE(ts.remove_version("a"));
    EXPECT_EQ("b", ts.select("x"));
}

TEST(TrafficSplitterTest, SetWeightChangesShare) {
    TrafficSplitter ts;
    ts.add_version("a", 1);
    ts.add_version("b", 1);
    ts.set_weight("a", 3);
    auto vs = ts.versions();
    double a_share = 0;
    for (const auto& v : vs) {
        if (v.version == "a") a_share = v.share;
    }
    EXPECT_NEAR(0.75, a_share, 1e-6);
}

TEST(TrafficSplitterTest, DistributionOverKeys) {
    TrafficSplitter ts;
    ts.add_version("stable", 80);
    ts.add_version("canary", 20);
    int stable = 0, canary = 0;
    for (int i = 0; i < 1000; ++i) {
        if (ts.select("u" + std::to_string(i)) == "stable") ++stable;
        else ++canary;
    }
    // 粗粒度：canary 应占一定比例，不至于为 0 或全部
    EXPECT_GT(canary, 50);
    EXPECT_LT(canary, 500);
}

// ==================== ConfigVersionStore ====================

TEST(ConfigVersionStoreTest, PutActivateGet) {
    ConfigVersionStore store;
    store.put_version("v1", {{"timeout", "100"}, {"log", "info"}});
    store.put_version("v2", {{"timeout", "200"}, {"log", "debug"}});

    EXPECT_EQ("v1", store.active());  // 首个版本自动生效
    EXPECT_EQ("100", store.get("timeout"));

    ASSERT_TRUE(store.activate("v2"));
    EXPECT_EQ("v2", store.active());
    EXPECT_EQ("200", store.get("timeout"));
    EXPECT_EQ("debug", store.get("log"));
}

TEST(ConfigVersionStoreTest, ActivateMissingFails) {
    ConfigVersionStore store;
    store.put_version("v1", {{"a", "1"}});
    EXPECT_FALSE(store.activate("nope"));
    EXPECT_EQ("v1", store.active());
}

TEST(ConfigVersionStoreTest, MissingKeyReturnsDefault) {
    ConfigVersionStore store;
    store.put_version("v1", {{"a", "1"}});
    store.activate("v1");
    EXPECT_EQ("fallback", store.get("missing", "fallback"));
}

TEST(ConfigVersionStoreTest, VersionCount) {
    ConfigVersionStore store;
    store.put_version("v1", {});
    store.put_version("v2", {});
    EXPECT_EQ(2u, store.version_count());
    EXPECT_TRUE(store.has_version("v2"));
    EXPECT_FALSE(store.has_version("v3"));
}
