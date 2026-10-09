#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <chrono>
#include <filesystem>

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

class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const char* value) : name_(name) {
        const char* previous = std::getenv(name);
        had_previous_ = previous != nullptr;
        if (previous) previous_ = previous;
        assign(value);
    }
    ~ScopedEnvironment() { assign(had_previous_ ? previous_.c_str() : nullptr); }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
private:
    void assign(const char* value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value ? value : "");
#else
        if (value) setenv(name_.c_str(), value, 1);
        else unsetenv(name_.c_str());
#endif
    }
    std::string name_, previous_;
    bool had_previous_;
};

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

    // 显式推进 mtime，兼容秒级 stat 且无需等待文件系统时钟。
    const auto changed_time = std::filesystem::last_write_time(p) + std::chrono::seconds(2);
    {
        std::ofstream out(p.c_str());
        out << R"({"v": "new"})";
        out.flush();
    }
    std::filesystem::last_write_time(p, changed_time);
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

TEST(ConfigTest, FailedJsonLoadPreservesActiveConfiguration) {
    auto good = write_temp("active.json", R"({"listen_port": 9100, "component.chat.enabled": false})");
    auto bad = write_temp("partial.json", R"({"listen_port": 9200, "component.chat.enabled": true, broken})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_file(good));
    EXPECT_FALSE(cfg.load_json_from_file(bad));
    EXPECT_EQ(9100, cfg.listen_port());
    EXPECT_EQ("9100", cfg.get_string("listen_port"));
    EXPECT_FALSE(cfg.is_component_enabled("chat"));
    EXPECT_EQ(std::vector<std::string>({good}), cfg.loaded_files());
    std::remove(good.c_str());
    std::remove(bad.c_str());
}

TEST(ConfigTest, InvalidOverlayDoesNotPublishPartialConfiguration) {
    auto base = write_temp("atomic_base.json", R"({"v": "base"})");
    auto bad = write_temp("atomic_overlay.json", R"({"v": "partial", broken})");
    core::Config cfg;
    cfg.set("v", "active");
    EXPECT_FALSE(cfg.load_json_from_files({base, bad}));
    EXPECT_EQ("active", cfg.get_string("v"));
    EXPECT_TRUE(cfg.loaded_files().empty());
    std::remove(base.c_str());
    std::remove(bad.c_str());
}

TEST(ConfigTest, MissingFilesDoNotReplaceActiveConfiguration) {
    core::Config cfg;
    cfg.set("v", "active");
    auto good = write_temp("missing_base.conf", "v=new\n");
    const std::string missing = "cfg_test_missing_file.conf";
    std::remove(missing.c_str());
    EXPECT_FALSE(cfg.load_from_files({good, missing}));
    EXPECT_EQ("active", cfg.get_string("v"));
    EXPECT_FALSE(cfg.load_json_from_file(missing));
    EXPECT_EQ("active", cfg.get_string("v"));
    EXPECT_FALSE(cfg.load_from_files({}));
    EXPECT_EQ("active", cfg.get_string("v"));
    std::remove(good.c_str());
}

TEST(ConfigTest, MixedFormatsRespectEnvironmentLayerOrder) {
    const std::string dir = "cfg_mixed_env_dir";
    std::filesystem::create_directory(dir);
    { std::ofstream out(dir + "/default.conf"); out << "mode=default-conf\nconf_only=kept\n"; }
    { std::ofstream out(dir + "/default.json"); out << R"({"mode": "default-json", "json_only": "kept"})"; }
    { std::ofstream out(dir + "/prod.conf"); out << "mode=prod-conf\n"; }
    core::Config cfg;
    ASSERT_TRUE(cfg.load_for_env(dir, "prod"));
    EXPECT_EQ("prod-conf", cfg.get_string("mode"));
    EXPECT_EQ("kept", cfg.get_string("conf_only"));
    EXPECT_EQ("kept", cfg.get_string("json_only"));
    ASSERT_TRUE(cfg.reload());
    EXPECT_EQ("prod-conf", cfg.get_string("mode"));
    std::remove((dir + "/default.conf").c_str());
    std::remove((dir + "/default.json").c_str());
    std::remove((dir + "/prod.conf").c_str());
    std::filesystem::remove(dir);
}

TEST(ConfigTest, FailedMixedReloadKeepsAllLayersAndDoesNotNotify) {
    const std::string dir = "cfg_atomic_env_dir";
    std::filesystem::create_directory(dir);
    { std::ofstream out(dir + "/default.conf"); out << "mode=old\n"; }
    { std::ofstream out(dir + "/prod.json"); out << R"({"env": "old"})"; }
    core::Config cfg;
    ASSERT_TRUE(cfg.load_for_env(dir, "prod"));
    const auto files = cfg.loaded_files();
    int changes = 0;
    cfg.add_change_listener([&] { ++changes; });
    { std::ofstream out(dir + "/default.conf"); out << "mode=new\n"; }
    { std::ofstream out(dir + "/prod.json"); out << R"({"env": "partial", broken})"; }
    EXPECT_FALSE(cfg.reload());
    EXPECT_EQ("old", cfg.get_string("mode"));
    EXPECT_EQ("old", cfg.get_string("env"));
    EXPECT_EQ(files, cfg.loaded_files());
    EXPECT_EQ(0, changes);
    std::remove((dir + "/default.conf").c_str());
    std::remove((dir + "/prod.json").c_str());
    std::filesystem::remove(dir);
}

TEST(ConfigTest, FailedHotReloadRetriesWithoutAnotherTimestampChange) {
    auto p = write_temp("retry.json", R"({"v": "old"})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_file(p));
    int changes = 0;
    cfg.add_change_listener([&] { ++changes; EXPECT_EQ("new", cfg.get_string("v")); });
    const auto changed_time = std::filesystem::last_write_time(p) + std::chrono::seconds(2);
    { std::ofstream out(p); out << R"({"v": "partial", broken})"; }
    std::filesystem::last_write_time(p, changed_time);
    EXPECT_FALSE(cfg.check_reload());
    EXPECT_EQ("old", cfg.get_string("v"));
    EXPECT_EQ(0, changes);
    { std::ofstream out(p); out << R"({"v": "new"})"; }
    std::filesystem::last_write_time(p, changed_time);
    EXPECT_TRUE(cfg.check_reload());
    EXPECT_EQ("new", cfg.get_string("v"));
    EXPECT_EQ(1, changes);
    EXPECT_FALSE(cfg.check_reload());
    std::remove(p.c_str());
}

TEST(ConfigTest, RemovedFieldsAndRollbackRestoreDefaults) {
    auto p = write_temp("defaults.json", R"({"listen_port": 9100, "worker_threads": 8})");
    core::Config cfg;
    cfg.snapshot();
    ASSERT_TRUE(cfg.load_json_from_file(p));
    EXPECT_EQ(9100, cfg.listen_port());
    EXPECT_EQ(8, cfg.worker_threads());
    ASSERT_TRUE(cfg.rollback());
    EXPECT_EQ(9000, cfg.listen_port());
    EXPECT_EQ(4, cfg.worker_threads());
    ASSERT_TRUE(cfg.load_json_from_file(p));
    { std::ofstream out(p); out << "{}"; }
    ASSERT_TRUE(cfg.reload());
    EXPECT_EQ(9000, cfg.listen_port());
    EXPECT_EQ(4, cfg.worker_threads());
    std::remove(p.c_str());
}

TEST(ConfigTest, OverlayTracksBaseFileForReload) {
    auto base = write_temp("conf_base.conf", "mode=base\nbase_only=old\n");
    auto overlay = write_temp("json_overlay.json", R"({"mode": "overlay"})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_from_file(base));
    ASSERT_TRUE(cfg.load_json_from_files({overlay}, false));
    EXPECT_EQ(std::vector<std::string>({base, overlay}), cfg.loaded_files());
    { std::ofstream out(base); out << "mode=changed\nbase_only=new\n"; }
    ASSERT_TRUE(cfg.reload());
    EXPECT_EQ("overlay", cfg.get_string("mode"));
    EXPECT_EQ("new", cfg.get_string("base_only"));
    std::remove(base.c_str());
    std::remove(overlay.c_str());
}

TEST(ConfigTest, ReloadPreservesExplicitFileFormat) {
    auto p = write_temp("json_without_extension", R"({"v": "old"})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_file(p));
    { std::ofstream out(p); out << R"({"v": "new"})"; }
    ASSERT_TRUE(cfg.reload());
    EXPECT_EQ("new", cfg.get_string("v"));
    std::remove(p.c_str());
}

class InvalidConfigJsonTest : public ::testing::TestWithParam<const char*> {};

TEST_P(InvalidConfigJsonTest, RejectsInvalidInputWithoutChangingConfiguration) {
    auto p = write_temp("invalid_syntax.json", GetParam());
    core::Config cfg;
    cfg.set("active", "kept");
    EXPECT_FALSE(cfg.load_json_from_file(p)) << GetParam();
    EXPECT_EQ("kept", cfg.get_string("active"));
    EXPECT_TRUE(cfg.loaded_files().empty());
    std::remove(p.c_str());
}

INSTANTIATE_TEST_SUITE_P(StrictJson, InvalidConfigJsonTest, ::testing::Values(
    R"(prefix {"v": 1})", R"({"v": 1} suffix)", R"({"v": 1,})",
    R"({"v": 1 "other": 2})", R"({"v": })", R"({"v": invalid})",
    R"({"v": 01})", R"({"v": +1})", R"({"v": 1.})", R"({"v": .5})",
    R"({"v": 1e})", R"({"v": truejunk})", R"({"v": [1, 2]})",
    R"({"v": "bad\q"})", R"({"v": "bad\u12xy"})",
    R"({"v": "bad\uD800"})", R"({"v": "bad\uDC00"})",
    "{\"v\": \"raw\nnewline\"}", R"({"nested": {"v": 1,}})"
));

TEST(ConfigTest, JsonEscapesAndBracesInsideStrings) {
    auto p = write_temp("escapes.json",
        R"({"nested": {"text": "brace } and {, quote \" slash \/ backslash \\ newline \n"}, "a\"b": "\u4f60\u597d\ud83d\ude00", "n": -1.25e+2})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_file(p));
    EXPECT_EQ("brace } and {, quote \" slash / backslash \\ newline \n", cfg.get_string("nested.text"));
    EXPECT_EQ(u8"你好😀", cfg.get_string("a\"b"));
    EXPECT_EQ("-1.25e+2", cfg.get_string("n"));
    std::remove(p.c_str());
}

TEST(ConfigTest, JsonNestingDepthIsBounded) {
    std::string json = "{}";
    for (int i = 0; i < 100; ++i) json = "{\"nested\":" + json + "}";
    auto p = write_temp("deep.json", json);
    core::Config cfg;
    EXPECT_FALSE(cfg.load_json_from_file(p));
    std::remove(p.c_str());
}

TEST(ConfigTest, HotReloadBaselinesAreIndependentAcrossInstances) {
    auto p = write_temp("shared.json", R"({"v": "old"})");
    core::Config first, second;
    ASSERT_TRUE(first.load_json_from_file(p));
    ASSERT_TRUE(second.load_json_from_file(p));
    const auto changed_time = std::filesystem::last_write_time(p) + std::chrono::seconds(2);
    { std::ofstream out(p); out << R"({"v": "new"})"; }
    std::filesystem::last_write_time(p, changed_time);
    EXPECT_TRUE(first.check_reload());
    EXPECT_TRUE(second.check_reload());
    EXPECT_EQ("new", first.get_string("v"));
    EXPECT_EQ("new", second.get_string("v"));
    std::remove(p.c_str());
}

TEST(ConfigTest, DeletedOverlayDoesNotPublishBaseOnlyConfiguration) {
    auto base = write_temp("delete_base.json", R"({"v": "base"})");
    auto overlay = write_temp("delete_overlay.json", R"({"v": "overlay"})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_files({base, overlay}));
    std::remove(overlay.c_str());
    EXPECT_FALSE(cfg.check_reload());
    EXPECT_EQ("overlay", cfg.get_string("v"));
    EXPECT_EQ(std::vector<std::string>({base, overlay}), cfg.loaded_files());
    std::remove(base.c_str());
}

TEST(ConfigTest, EnvironmentOverridesSurviveSetAndRollback) {
    ScopedEnvironment port("CHWELL_LISTEN_PORT", "9700");
    ScopedEnvironment workers("CHWELL_WORKER_THREADS", "16");
    auto p = write_temp("env_override.json", R"({"listen_port": 9100, "worker_threads": 8})");
    core::Config cfg;
    ASSERT_TRUE(cfg.load_json_from_file(p));
    EXPECT_EQ(9700, cfg.listen_port());
    EXPECT_EQ(16, cfg.worker_threads());
    cfg.snapshot();
    cfg.set("unrelated", "value");
    EXPECT_EQ(9700, cfg.listen_port());
    EXPECT_EQ(16, cfg.worker_threads());
    cfg.set("listen_port", "9200");
    EXPECT_EQ(9700, cfg.listen_port());
    ASSERT_TRUE(cfg.rollback());
    EXPECT_EQ(9700, cfg.listen_port());
    EXPECT_EQ(16, cfg.worker_threads());
    std::remove(p.c_str());
}

}  // namespace
