#include <gtest/gtest.h>

#include "chwell/service/hot_reload.h"
#include "chwell/service/plugin.h"
#include "chwell/service/service.h"

using namespace chwell;

#ifndef TEST_HOT_PLUGIN_PATH
#define TEST_HOT_PLUGIN_PATH ""
#endif

namespace {

TEST(HotReloadTest, MissingFileFails) {
    service::PluginManager pm;
    service::HotReloadManager hot(&pm, nullptr);
    EXPECT_FALSE(hot.load("/nonexistent/definitely_missing_plugin.so"));
    EXPECT_FALSE(hot.is_loaded("/nonexistent/definitely_missing_plugin.so"));
}

TEST(HotReloadTest, LoadAndUnloadPlugin) {
    service::PluginManager pm;
    service::HotReloadManager hot(&pm, nullptr);

    ASSERT_TRUE(hot.load(TEST_HOT_PLUGIN_PATH));
    EXPECT_TRUE(hot.is_loaded(TEST_HOT_PLUGIN_PATH));

    auto paths = hot.loaded_paths();
    ASSERT_EQ(1u, paths.size());
    EXPECT_EQ(TEST_HOT_PLUGIN_PATH, paths[0]);

    // 重复加载应失败
    EXPECT_FALSE(hot.load(TEST_HOT_PLUGIN_PATH));

    EXPECT_TRUE(hot.unload(TEST_HOT_PLUGIN_PATH));
    EXPECT_FALSE(hot.is_loaded(TEST_HOT_PLUGIN_PATH));

    // 重复卸载应失败
    EXPECT_FALSE(hot.unload(TEST_HOT_PLUGIN_PATH));
}

TEST(HotReloadTest, ReloadPlugin) {
    service::PluginManager pm;
    service::HotReloadManager hot(&pm, nullptr);

    ASSERT_TRUE(hot.load(TEST_HOT_PLUGIN_PATH));
    EXPECT_TRUE(hot.reload(TEST_HOT_PLUGIN_PATH));
    EXPECT_TRUE(hot.is_loaded(TEST_HOT_PLUGIN_PATH));

    // 重载后仍可卸载
    EXPECT_TRUE(hot.unload(TEST_HOT_PLUGIN_PATH));
}

TEST(HotReloadTest, ReloadNonLoadedFails) {
    service::PluginManager pm;
    service::HotReloadManager hot(&pm, nullptr);
    EXPECT_FALSE(hot.reload(TEST_HOT_PLUGIN_PATH));
}

TEST(HotReloadTest, CheckUpdatesNoChange) {
    service::PluginManager pm;
    service::HotReloadManager hot(&pm, nullptr);
    ASSERT_TRUE(hot.load(TEST_HOT_PLUGIN_PATH));
    // 未改文件时不应触发重载
    EXPECT_EQ(0, hot.check_updates());
    EXPECT_TRUE(hot.unload(TEST_HOT_PLUGIN_PATH));
}

TEST(HotReloadTest, AllowedPrefixRejects) {
    service::PluginManager pm;
    service::HotReloadManager hot(&pm, nullptr);
    hot.set_allowed_prefix("/opt/plugins/");
    EXPECT_FALSE(hot.load(TEST_HOT_PLUGIN_PATH));  // 路径不在允许前缀内
}

TEST(HotReloadTest, MultiPluginIsolation) {
    service::PluginManager pm;
    service::HotReloadManager hot(&pm, nullptr);
    ASSERT_TRUE(hot.load(TEST_HOT_PLUGIN_PATH));
    // 同一 so 加载两次不允许；卸载后再加载成功
    EXPECT_FALSE(hot.load(TEST_HOT_PLUGIN_PATH));
    EXPECT_TRUE(hot.unload(TEST_HOT_PLUGIN_PATH));
    EXPECT_TRUE(hot.load(TEST_HOT_PLUGIN_PATH));
    EXPECT_TRUE(hot.unload(TEST_HOT_PLUGIN_PATH));
}

class RejectHotComponent : public service::Component {
public:
    explicit RejectHotComponent(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }
private:
    std::string name_;
};

TEST(HotReloadTest, InactiveUnloadCancelsOwnedCallbacksAndDestroysComponentsBeforeImage) {
    service::Service service(0, 1);
    service::HotReloadManager hot(&service.plugin_manager(), &service);
    ASSERT_TRUE(hot.load(TEST_HOT_PLUGIN_PATH));
    EXPECT_EQ(service.component_count(), 1u);
    EXPECT_TRUE(hot.unload(TEST_HOT_PLUGIN_PATH)); // Fixture rejects Uninstall if callback was not cancelled.
    EXPECT_EQ(service.component_count(), 0u);
    EXPECT_TRUE(hot.load(TEST_HOT_PLUGIN_PATH));
    EXPECT_TRUE(hot.unload(TEST_HOT_PLUGIN_PATH));
}

TEST(HotReloadTest, ServiceOwnsRegistrationsAndRejectsLiveComponentReplacement) {
    service::Service service(0, 1);
    service::HotReloadManager hot(&service.plugin_manager(), &service);
    ASSERT_TRUE(hot.load(TEST_HOT_PLUGIN_PATH));
    auto* component = service.get_component("hot-owned");
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(service.component_owner(component), "TestHotPlugin");
    EXPECT_FALSE(hot.reload(TEST_HOT_PLUGIN_PATH));
    ASSERT_TRUE(service.start_checked());
    EXPECT_FALSE(hot.unload(TEST_HOT_PLUGIN_PATH));
    EXPECT_FALSE(hot.reload(TEST_HOT_PLUGIN_PATH));
    EXPECT_EQ(service.get_component("hot-owned"), component);
    service.stop();
    EXPECT_EQ(service.component_count(), 0u);
    EXPECT_TRUE(hot.unload(TEST_HOT_PLUGIN_PATH));
}

TEST(HotReloadTest, PartialInstallFailureAndExceptionRemoveOwnedComponents) {
    for (const auto* marker : {"reject-hot-install", "throw-hot-install"}) {
        service::Service service(0, 1);
        ASSERT_NE(service.add_component<RejectHotComponent>(marker), nullptr);
        service::HotReloadManager hot(&service.plugin_manager(), &service);
        EXPECT_FALSE(hot.load(TEST_HOT_PLUGIN_PATH));
        EXPECT_FALSE(hot.is_loaded(TEST_HOT_PLUGIN_PATH));
        EXPECT_EQ(service.get_component("hot-owned"), nullptr);
        EXPECT_EQ(service.component_count(), 1u);
    }
}

TEST(HotReloadTest, DestroyingManagerWhileRunningKeepsImagePinnedUntilServiceStop) {
    service::Service service(0, 1);
    {
        service::HotReloadManager hot(&service.plugin_manager(), &service);
        ASSERT_TRUE(hot.load(TEST_HOT_PLUGIN_PATH));
        ASSERT_TRUE(service.start_checked());
    }
    auto* component = service.get_component("hot-owned");
    ASSERT_NE(component, nullptr);
    EXPECT_TRUE(component->Update(1)); // Calls a vtable in the retained shared library.
    service.stop();
    EXPECT_EQ(service.get_component("hot-owned"), nullptr);
}

TEST(HotReloadTest, ServiceDestructionBeforeManagerLeavesNoDanglingServiceAccess) {
    auto service = std::make_unique<service::Service>(0, 1);
    service::HotReloadManager hot(&service->plugin_manager(), service.get());
    ASSERT_TRUE(hot.load(TEST_HOT_PLUGIN_PATH));
    service.reset();
    EXPECT_FALSE(hot.load(TEST_HOT_PLUGIN_PATH));
    EXPECT_TRUE(hot.unload(TEST_HOT_PLUGIN_PATH));
}

}  // namespace
