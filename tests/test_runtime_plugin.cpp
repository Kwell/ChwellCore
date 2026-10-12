#include <gtest/gtest.h>
#include "chwell/service/runtime_plugin.h"
#include "chwell/service/runtime_plugin_abi.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace chwell::service;
namespace fs = std::filesystem;
namespace {
class RuntimePluginTest : public ::testing::Test {
protected:
    fs::path root, source;
    void SetUp() override {
        static std::atomic<unsigned> sequence{0};
        root = fs::temp_directory_path() / ("chwell-runtime-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence));
        fs::create_directory(root);
        source = root / fs::path(RUNTIME_PLUGIN_V1).filename();
    }
    void TearDown() override { std::error_code ignored; fs::remove_all(root, ignored); }
    void deploy(const fs::path& fixture) { fs::copy_file(fixture, source, fs::copy_options::overwrite_existing); }
    std::vector<fs::path> images() {
        std::vector<fs::path> result;
        for (const auto& item : fs::directory_iterator(root))
            if (item.path().filename().u8string().find(".chwell-plugin-") == 0) result.push_back(item.path());
        return result;
    }
    int reply(const std::shared_ptr<const RuntimePluginSnapshot>& snapshot) {
        std::vector<std::uint8_t> output;
        EXPECT_TRUE(snapshot->invoke({'x', 0, 255}, output));
        EXPECT_EQ(output.size(), 4u);
        return output.empty() ? -1 : output[0];
    }
};
}

TEST_F(RuntimePluginTest, SamePathPublishesDistinctVersionsAndOldReadersKeepTheirRelease) {
    RuntimePluginStore store("test.logic", root.u8string());
    deploy(RUNTIME_PLUGIN_V1);
    ASSERT_TRUE(store.publish(source.u8string(), 0));
    auto old = store.acquire();
    EXPECT_EQ(old->version(), 1u);
    EXPECT_EQ(old->generation(), 1u);
    EXPECT_EQ(reply(old), 1);
    deploy(RUNTIME_PLUGIN_V2);
    ASSERT_TRUE(store.publish(source.u8string(), 1));
    auto next = store.acquire();
    EXPECT_EQ(reply(next), 2);
    EXPECT_EQ(reply(old), 1);
    EXPECT_EQ(images().size(), 2u);
    ASSERT_TRUE(store.rollback(2));
    EXPECT_EQ(store.generation(), 3u);
    EXPECT_EQ(store.acquire()->generation(), 3u);
    EXPECT_EQ(reply(store.acquire()), 1);
    EXPECT_EQ(reply(next), 2);
    EXPECT_FALSE(store.rollback(3));
    EXPECT_FALSE(store.publish(source.u8string(), 3)); // High-water mark remains 2 after rollback.
    deploy(RUNTIME_PLUGIN_V3);
    ASSERT_TRUE(store.publish(source.u8string(), 3));
    EXPECT_EQ(reply(store.acquire()), 3);
    EXPECT_FALSE(store.clear(3));
    EXPECT_TRUE(store.clear(4));
    EXPECT_EQ(store.generation(), 5u);
    EXPECT_EQ(store.acquire(), nullptr);
    EXPECT_FALSE(store.rollback(5));
    EXPECT_EQ(reply(old), 1);
    EXPECT_EQ(reply(next), 2);
    old.reset(); next.reset();
    EXPECT_TRUE(images().empty());
    EXPECT_FALSE(store.publish(source.u8string(), 5)); // clear also retains release history.
}

TEST_F(RuntimePluginTest, BadCandidateLeavesActiveSnapshotAndCleansStagingFiles) {
    RuntimePluginStore store("test.logic", root.u8string());
    deploy(RUNTIME_PLUGIN_V1);
    ASSERT_TRUE(store.publish(source.u8string(), 0));
    auto current = store.acquire();
    for (const auto* bad : {RUNTIME_PLUGIN_WRONG_ABI, RUNTIME_PLUGIN_WRONG_NAME,
                          RUNTIME_PLUGIN_ZERO_VERSION, RUNTIME_PLUGIN_WRONG_SIZE}) {
        deploy(bad);
        std::string error;
        EXPECT_FALSE(store.publish(source.u8string(), 1, &error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(store.acquire(), current);
        EXPECT_EQ(store.generation(), 1u);
        EXPECT_EQ(images().size(), 1u);
        EXPECT_EQ(reply(current), 1);
    }
    { std::ofstream invalid(source, std::ios::binary | std::ios::trunc); invalid << "not a library"; }
    EXPECT_FALSE(store.publish(source.u8string(), 1));
    EXPECT_EQ(images().size(), 1u);
    EXPECT_FALSE(store.publish((root / "absent").u8string(), 1));
    EXPECT_EQ(store.acquire(), current);
}

TEST_F(RuntimePluginTest, CanonicalContainmentRejectsSiblingTraversalAndSymlinkEscape) {
    RuntimePluginStore store("test.logic", root.u8string());
    const auto sibling = fs::path(root.u8string() + "-sibling");
    fs::create_directory(sibling);
    auto outside = sibling / source.filename();
    fs::copy_file(RUNTIME_PLUGIN_V1, outside);
    EXPECT_FALSE(store.publish(outside.u8string(), 0));
    EXPECT_FALSE(store.publish((root / ".." / sibling.filename() / source.filename()).u8string(), 0));
    std::error_code permission;
    fs::create_symlink(outside, source, permission);
    if (!permission) EXPECT_FALSE(store.publish(source.u8string(), 0));
    fs::remove_all(sibling);
    EXPECT_TRUE(images().empty());
    EXPECT_EQ(store.generation(), 0u);
}

TEST_F(RuntimePluginTest, InvocationFailureAndLimitsLeaveOutputUntouched) {
    RuntimePluginStore store("test.logic", root.u8string());
    deploy(RUNTIME_PLUGIN_V1);
    ASSERT_TRUE(store.publish(source.u8string(), 0));
    std::vector<std::uint8_t> output{9, 8};
    for (const auto& request : {std::vector<std::uint8_t>{'E'}, std::vector<std::uint8_t>{'L'},
                              std::vector<std::uint8_t>(1024 * 1024 + 1)}) {
        EXPECT_FALSE(store.acquire()->invoke(request, output));
        EXPECT_EQ(output, (std::vector<std::uint8_t>{9, 8}));
    }
    EXPECT_FALSE(store.acquire()->invoke({}, output, nullptr, 0));
    EXPECT_FALSE(store.acquire()->invoke({}, output, nullptr, 1024 * 1024 + 1));
    EXPECT_TRUE(store.acquire()->invoke({}, output));
    EXPECT_EQ(output, (std::vector<std::uint8_t>{1}));
}

TEST_F(RuntimePluginTest, ConcurrentPublishHasExactlyOneWinnerAndStaleWriterDoesNotLoad) {
    RuntimePluginStore store("test.logic", root.u8string());
    deploy(RUNTIME_PLUGIN_V1);
    ASSERT_TRUE(store.publish(source.u8string(), 0));
    deploy(RUNTIME_PLUGIN_V2);
    std::promise<void> start;
    auto gate = start.get_future().share();
    auto publish = [&] { gate.wait(); return store.publish(source.u8string(), 1); };
    auto a = std::async(std::launch::async, publish), b = std::async(std::launch::async, publish);
    start.set_value();
    EXPECT_NE(a.get(), b.get());
    EXPECT_EQ(store.generation(), 2u);
    EXPECT_EQ(reply(store.acquire()), 2);
    EXPECT_EQ(images().size(), 2u);
    EXPECT_FALSE(store.publish("not-even-a-file", 1));
}

TEST_F(RuntimePluginTest, RetainedSnapshotOutlivesStore) {
    std::shared_ptr<const RuntimePluginSnapshot> snapshot;
    deploy(RUNTIME_PLUGIN_V1);
    {
        RuntimePluginStore store("test.logic", root.u8string());
        ASSERT_TRUE(store.publish(source.u8string(), 0));
        snapshot = store.acquire();
    }
    EXPECT_EQ(images().size(), 1u);
    EXPECT_EQ(reply(snapshot), 1);
    snapshot.reset();
    EXPECT_TRUE(images().empty());
}

TEST_F(RuntimePluginTest, InFlightCallSurvivesPublishClearAndOwnerDestruction) {
    deploy(RUNTIME_PLUGIN_V1);
    auto store = std::make_unique<RuntimePluginStore>("test.logic", root.u8string());
    ASSERT_TRUE(store->publish(source.u8string(), 0));
    auto copied = images();
    ASSERT_EQ(copied.size(), 1u);
    // Resolve deterministic gates, then drop our loader handle. Only the actual
    // invocation's retained snapshot pins the old image across publication/clear.
#if defined(_WIN32)
    auto library = LoadLibraryW(copied[0].c_str());
    ASSERT_NE(library, nullptr);
    auto entered = reinterpret_cast<int (CHWELL_PLUGIN_CALL*)()>(GetProcAddress(library, "chwell_test_entered"));
    auto release = reinterpret_cast<void (CHWELL_PLUGIN_CALL*)()>(GetProcAddress(library, "chwell_test_release"));
    FreeLibrary(library);
#else
    auto library = dlopen(copied[0].c_str(), RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(library, nullptr);
    auto entered = reinterpret_cast<int (*)()>(dlsym(library, "chwell_test_entered"));
    auto release = reinterpret_cast<void (*)()>(dlsym(library, "chwell_test_release"));
    dlclose(library);
#endif
    ASSERT_NE(entered, nullptr); ASSERT_NE(release, nullptr);
    auto call = std::async(std::launch::async, [snapshot = store->acquire()] {
        std::vector<std::uint8_t> output;
        const bool ok = snapshot->invoke({'B'}, output);
        return std::make_pair(ok, output);
    });
    // Ensure any failing assertion releases the blocked invocation before waiting.
    struct Gate { decltype(release) function; ~Gate() { if (function) function(); } } gate{release};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    ASSERT_TRUE(entered());
    deploy(RUNTIME_PLUGIN_V2);
    ASSERT_TRUE(store->publish(source.u8string(), 1));
    EXPECT_EQ(reply(store->acquire()), 2);
    ASSERT_TRUE(store->clear(2));
    store.reset();
    EXPECT_TRUE(fs::exists(copied[0]));
    release(); gate.function = nullptr;
    const auto result = call.get();
    EXPECT_TRUE(result.first);
    EXPECT_EQ(result.second, (std::vector<std::uint8_t>{1, 'B'}));
    EXPECT_TRUE(images().empty());
}
