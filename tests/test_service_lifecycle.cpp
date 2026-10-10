#include <gtest/gtest.h>
#include <stdexcept>
#include <future>
#include "chwell/service/service.h"

using namespace chwell::service;

namespace {
class RecordingComponent : public Component {
public:
    RecordingComponent(std::string name, std::vector<std::string>& events, int priority = 100,
                       std::string* failure = nullptr, bool throws = false)
        : name_(std::move(name)), events_(events), priority_(priority), failure_(failure), throws_(throws) {}
    std::string name() const override { return name_; }
    int priority() const override { return priority_; }
    bool Init() override { return phase("Init"); }
    bool PostInit() override { return phase("PostInit"); }
    bool CheckConfig() override { return phase("CheckConfig"); }
    bool PreUpdate() override { return phase("PreUpdate"); }
    bool PreShut() override { return phase("PreShut"); }
    bool Shut() override { return phase("Shut"); }
private:
    bool phase(const std::string& phase) {
        events_.push_back(name_ + ":" + phase);
        if (failure_ && *failure_ == phase) {
            if (throws_) throw std::runtime_error("test lifecycle failure");
            return false;
        }
        return true;
    }
    std::string name_;
    std::vector<std::string>& events_;
    int priority_;
    std::string* failure_;
    bool throws_;
};

class RecordingPlugin : public IPlugin {
public:
    RecordingPlugin(std::string name, std::vector<std::string>& events, bool* fail = nullptr,
                    bool throws = false, std::string component_name = "")
        : name_(std::move(name)), events_(events), fail_(fail), throws_(throws),
          component_name_(component_name.empty() ? name_ : std::move(component_name)) {}
    const std::string& GetName() const override { return name_; }
    bool Install(Service& service) override {
        events_.push_back(name_ + ":Install");
        service.add_component<RecordingComponent>(component_name_, events_);
        if (throws_) throw std::runtime_error("test install failure");
        return !fail_ || !*fail_;
    }
    bool Uninstall(Service&) override {
        events_.push_back(name_ + ":Uninstall");
        return true;
    }
private:
    std::string name_;
    std::vector<std::string>& events_;
    bool* fail_;
    bool throws_;
    std::string component_name_;
};
} // namespace

TEST(ServiceLifecycleTest, InitFailureUnwindsAttemptedComponentsInReverseOrder) {
    std::vector<std::string> events;
    std::string failure = "Init";
    Service service(0, 1);
    service.add_component<RecordingComponent>("a", events, 1);
    service.add_component<RecordingComponent>("b", events, 2, &failure);
    service.add_component<RecordingComponent>("c", events, 3);
    EXPECT_FALSE(service.start_checked());
    EXPECT_FALSE(service.is_running());
    EXPECT_EQ(events, (std::vector<std::string>{"a:Init", "b:Init", "b:PreShut",
                                               "a:PreShut", "b:Shut", "a:Shut"}));
    service.stop();
    EXPECT_EQ(events.size(), 6u);
}

TEST(ServiceLifecycleTest, LaterFailuresAndExceptionsUnwindAllInitializedComponents) {
    for (const auto& phase : {"PostInit", "CheckConfig", "PreUpdate"}) {
        for (bool throws : {false, true}) {
            SCOPED_TRACE(phase);
            std::vector<std::string> events;
            std::string failure = phase;
            Service service(0, 1);
            service.add_component<RecordingComponent>("a", events, 1);
            service.add_component<RecordingComponent>("b", events, 2, &failure, throws);
            service.add_component<RecordingComponent>("c", events, 3);
            EXPECT_FALSE(service.start_checked());
            ASSERT_GE(events.size(), 6u);
            EXPECT_EQ(std::vector<std::string>(events.end() - 6, events.end()),
                (std::vector<std::string>{"c:PreShut", "b:PreShut", "a:PreShut",
                                          "c:Shut", "b:Shut", "a:Shut"}));
        }
    }
}

TEST(ServiceLifecycleTest, ThrowingInitAndCleanupDoNotSkipEarlierComponents) {
    std::vector<std::string> events;
    std::string failure = "Init";
    std::string cleanup_failure = "Shut";
    Service service(0, 1);
    service.add_component<RecordingComponent>("a", events, 1, &cleanup_failure, true);
    service.add_component<RecordingComponent>("b", events, 2, &failure, true);
    EXPECT_FALSE(service.start_checked());
    EXPECT_EQ(events, (std::vector<std::string>{"a:Init", "b:Init", "b:PreShut",
                                               "a:PreShut", "b:Shut", "a:Shut"}));
    const auto count = events.size();
    service.stop();
    EXPECT_EQ(events.size(), count);
}

TEST(ServiceLifecycleTest, RetryAfterLifecycleFailureAndRepeatedStartAreSafe) {
    std::vector<std::string> events;
    std::string failure = "Init";
    Service service(0, 1);
    service.add_component<RecordingComponent>("a", events, 1, &failure);
    EXPECT_FALSE(service.start_checked());
    failure.clear();
    ASSERT_TRUE(service.start_checked());
    const auto size = events.size();
    EXPECT_TRUE(service.start_checked());
    EXPECT_EQ(events.size(), size);
    EXPECT_EQ(service.add_component<RecordingComponent>("late", events), nullptr);
    service.stop();
    const auto stopped_size = events.size();
    service.stop();
    EXPECT_EQ(events.size(), stopped_size);
    EXPECT_FALSE(service.start_checked()); // Rebuild the host after network shutdown.
}

TEST(ServiceLifecycleTest, RejectsDuplicateComponentNames) {
    std::vector<std::string> events;
    Service service(0, 1);
    auto* first = service.add_component<RecordingComponent>("same", events);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(service.add_component<RecordingComponent>("same", events), nullptr);
    EXPECT_EQ(service.get_component("same"), first);
    EXPECT_EQ(service.component_count(), 1u);
}

TEST(ServiceLifecycleTest, LegacyShutdownWaitsForInFlightWorkBeforeComponentCleanup) {
    class DrainComponent : public Component {
    public:
        DrainComponent(std::atomic<bool>& exited, bool& observed)
            : exited_(exited), observed_(observed) {}
        std::string name() const override { return "drain"; }
        bool Shut() override { observed_ = exited_.load(); return true; }
    private:
        std::atomic<bool>& exited_;
        bool& observed_;
    };
    std::atomic<bool> exited{false};
    bool observed = false;
    std::promise<void> entered;
    std::promise<void> release;
    auto entered_future = entered.get_future();
    auto release_future = release.get_future();
    Service service(0, 1);
    service.add_component<DrainComponent>(exited, observed);
    service.io_service().post([&] {
        entered.set_value();
        release_future.wait_for(std::chrono::seconds(5));
        exited = true;
    });
    ASSERT_TRUE(service.start_checked());
    ASSERT_EQ(entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto stopping = std::async(std::launch::async, [&] { service.stop(); });
    EXPECT_EQ(stopping.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
    release.set_value();
    stopping.get();
    EXPECT_TRUE(observed);
}

TEST(ServiceLifecycleTest, RegistrationExceptionCleansPartialResourcesAndRemovesComponent) {
    class ThrowingRegistration : public RecordingComponent {
    public:
        explicit ThrowingRegistration(std::vector<std::string>& events)
            : RecordingComponent("partial", events) {}
        void on_register(Service&) override { throw std::runtime_error("registration failed"); }
    };
    std::vector<std::string> events;
    Service service(0, 1);
    EXPECT_THROW(service.add_component<ThrowingRegistration>(events), std::runtime_error);
    EXPECT_EQ(service.component_count(), 0u);
    EXPECT_EQ(events, (std::vector<std::string>{"partial:PreShut", "partial:Shut"}));
}

TEST(ServiceLifecycleTest, BindFailureIsReportedAndComponentsAreCleaned) {
    std::vector<std::string> events;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    struct SocketGuard { int fd; ~SocketGuard() { ::close(fd); } } guard{fd};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(fd, 1), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
    for (bool epoll : {false, true}) {
        Service service(ntohs(address.sin_port), 1, epoll);
        service.add_component<RecordingComponent>("a", events);
        EXPECT_FALSE(service.start_checked());
        EXPECT_FALSE(service.is_running());
        EXPECT_EQ(events.back(), "a:Shut");
    }
}

TEST(PluginManagerTest, RollsBackFailedInstallAndAllowsRetry) {
    std::vector<std::string> events;
    bool fail = true;
    Service service(0, 1);
    service.add_component<RecordingComponent>("standalone", events, 0);
    auto& manager = service.plugin_manager();
    ASSERT_TRUE(manager.RegisterPlugin<RecordingPlugin>("one", events));
    ASSERT_TRUE(manager.RegisterPlugin<RecordingPlugin>("two", events, &fail));
    EXPECT_FALSE(manager.InstallAll(service));
    EXPECT_EQ(service.component_count(), 1u);
    EXPECT_EQ(events, (std::vector<std::string>{"one:Install", "two:Install", "two:PreShut",
        "two:Shut", "two:Uninstall", "one:PreShut", "one:Shut", "one:Uninstall"}));
    fail = false;
    ASSERT_TRUE(manager.InstallAll(service));
    EXPECT_EQ(service.component_owner(service.get_component("one")), "one");
    EXPECT_EQ(service.component_owner(service.get_component("standalone")), "");
    EXPECT_TRUE(manager.UninstallAll(service));
    EXPECT_EQ(service.component_count(), 1u);
    const auto count = events.size();
    EXPECT_TRUE(manager.UninstallAll(service));
    EXPECT_EQ(events.size(), count);
}

TEST(PluginManagerTest, RejectsDuplicateNullAndLatePlugins) {
    std::vector<std::string> events;
    Service service(0, 1);
    auto& manager = service.plugin_manager();
    EXPECT_FALSE(manager.RegisterPlugin(std::unique_ptr<IPlugin>{}));
    EXPECT_FALSE(manager.RegisterPlugin<RecordingPlugin>("", events));
    ASSERT_TRUE(manager.RegisterPlugin<RecordingPlugin>("one", events));
    EXPECT_FALSE(manager.RegisterPlugin<RecordingPlugin>("one", events));
    ASSERT_TRUE(manager.InstallAll(service));
    EXPECT_FALSE(manager.RegisterPlugin<RecordingPlugin>("late", events));
    EXPECT_TRUE(manager.UninstallAll(service));
}

TEST(PluginManagerTest, ExceptionsAndIgnoredComponentCollisionTriggerRollback) {
    for (bool throws : {false, true}) {
        std::vector<std::string> events;
        Service service(0, 1);
        service.add_component<RecordingComponent>("collision", events);
        auto& manager = service.plugin_manager();
        ASSERT_TRUE(manager.RegisterPlugin<RecordingPlugin>("one", events, nullptr, throws,
                                                            throws ? "owned" : "collision"));
        EXPECT_FALSE(manager.InstallAll(service));
        EXPECT_EQ(service.component_count(), 1u);
        EXPECT_EQ(service.get_component("owned"), nullptr);
        EXPECT_EQ(std::count(events.begin(), events.end(), "one:Uninstall"), 1);
    }
}

TEST(PluginManagerTest, OwnershipSurvivesComponentSortingAndShutdownHappensOnce) {
    std::vector<std::string> events;
    Service service(0, 1);
    auto& manager = service.plugin_manager();
    ASSERT_TRUE(manager.RegisterPlugin<RecordingPlugin>("owned", events));
    ASSERT_TRUE(manager.InstallAll(service));
    service.add_component<RecordingComponent>("standalone", events, -1);
    ASSERT_TRUE(service.start_checked());
    EXPECT_FALSE(manager.UninstallAll(service));
    service.stop();
    ASSERT_NE(service.get_component("standalone"), nullptr);
    EXPECT_EQ(service.get_component("owned"), nullptr);
    EXPECT_EQ(std::count(events.begin(), events.end(), "owned:Shut"), 1);
}
