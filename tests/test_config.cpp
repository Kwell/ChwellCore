#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <direct.h>
#define CHWELL_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define CHWELL_MKDIR(p) mkdir(p, 0755)
#endif

#include "chwell/core/config.h"

using namespace chwell;

namespace {

std::string write_temp(const std::string& name, const std::string& content) {
    std::string path = std::string("cfg_test_") + name;
    std::ofstream out(path.c_str());
    out << content;
    return path;
}

TEST(ConfigTest, JsonFlatParse) {
    auto p = write_temp("flat.json", R"({"listen_port": 9001, "server_name": "s1", "debug": true, "ratio": 0.5})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_file(p));
    EXPECT_EQ(9001, cfg.listen_port());
    EXPECT_EQ("s1", cfg.server_name());
    EXPECT_TRUE(cfg.get_bool("debug", false));
    EXPECT_EQ("0.5", cfg.get_string("ratio"));
    std::remove(p.c_str());
}

TEST(ConfigTest, JsonNestedExpand) {
    auto p = write_temp("nest.json", R"({"db": {"host": "127.0.0.1", "port": 3306}, "name": "x"})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_file(p));
    EXPECT_EQ("127.0.0.1", cfg.get_string("db.host"));
    EXPECT_EQ(3306, cfg.get_int("db.port", 0));
    EXPECT_EQ("x", cfg.get_string("name"));
    std::remove(p.c_str());
}

TEST(ConfigTest, RequireKeys) {
    auto p = write_temp("req.json", R"({"a": "1"})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_file(p));
    EXPECT_TRUE(cfg.require_keys({"a"}));
    EXPECT_FALSE(cfg.require_keys({"a", "missing"}));
    std::remove(p.c_str());
}

TEST(ConfigTest, SnapshotRollback) {
    auto p = write_temp("snap.json", R"({"x": "1"})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_file(p));
    EXPECT_EQ("1", cfg.get_string("x"));

    cfg.snapshot();
    cfg.set("x", "2");
    EXPECT_EQ("2", cfg.get_string("x"));

    ASSERT_TRUE(cfg.rollback());
    EXPECT_EQ("1", cfg.get_string("x"));

    // 无快照时回滚失败
    EXPECT_FALSE(cfg.rollback());
    std::remove(p.c_str());
}

TEST(ConfigTest, MultiFileOverlay) {
    auto a = write_temp("a.json", R"({"k": "1", "m": "x"})");
    auto b = write_temp("b.json", R"({"k": "2"})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_files({a, b}));
    EXPECT_EQ("2", cfg.get_string("k"));   // 后者覆盖
    EXPECT_EQ("x", cfg.get_string("m"));
    std::remove(a.c_str());
    std::remove(b.c_str());
}

TEST(ConfigTest, ReloadPicksUpChanges) {
    auto p = write_temp("hot.json", R"({"v": "old"})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_file(p));
    EXPECT_EQ("old", cfg.get_string("v"));

    int changed = 0;
    cfg.add_change_listener([&]() { ++changed; });

    // 修改文件
    {
        std::ofstream out(p.c_str());
        out << R"({"v": "new"})";
    }
    // mtime 粒度为秒，sleep 确保变化
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_TRUE(cfg.check_reload());
    EXPECT_EQ("new", cfg.get_string("v"));
    EXPECT_GE(changed, 1);

    // 无变化时 check_reload 返回 false
    EXPECT_FALSE(cfg.check_reload());
    std::remove(p.c_str());
}

TEST(ConfigTest, LoadedFilesTracked) {
    auto p = write_temp("track.json", R"({"a": "1"})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_file(p));
    auto files = cfg.loaded_files();
    ASSERT_EQ(1u, files.size());
    EXPECT_EQ(p, files[0]);
    std::remove(p.c_str());
}

TEST(ConfigTest, JsonParseRejectsGarbage) {
    auto p = write_temp("bad.json", "not json at all");
    core::Config cfg;
    EXPECT_FALSE(cfg.load_json_from_file(p));
    std::remove(p.c_str());
}

TEST(ConfigTest, ReloadReplacesKeysNotMerge) {
    auto p = write_temp("rep.json", R"({"keep": "1", "gone": "x"})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_file(p));
    EXPECT_EQ("x", cfg.get_string("gone"));

    {
        std::ofstream out(p.c_str());
        out << R"({"keep": "2"})";
    }
    ASSERT_TRUE(cfg.reload());
    EXPECT_EQ("2", cfg.get_string("keep"));
    // 已删除的键不应残留
    EXPECT_EQ("", cfg.get_string("gone", ""));
    std::remove(p.c_str());
}

TEST(ConfigTest, EnvProfileOverlay) {
    // default + prod 两层，放到独立子目录避免与 cwd 下其他文件冲突
    CHWELL_MKDIR("cfg_env_dir");
    {
        std::ofstream d("cfg_env_dir/default.json");
        d << R"({"mode": "dev", "port": 1000})";
    }
    {
        std::ofstream e("cfg_env_dir/prod.json");
        e << R"({"mode": "prod"})";
    }
    core::Config cfg;
    ASSERT_TRUE(cfg.load_for_env("cfg_env_dir", "prod"));
    EXPECT_EQ("prod", cfg.get_string("mode"));  // env 覆盖 default
    EXPECT_EQ(1000, cfg.get_int("port", 0));    // default 保留
    std::remove("cfg_env_dir/default.json");
    std::remove("cfg_env_dir/prod.json");
}

TEST(ConfigTest, CheckReloadWithoutPriorLoadDoesNothing) {
    core::Config cfg;
    EXPECT_FALSE(cfg.check_reload());
}

}  // namespace
