#include <gtest/gtest.h>
#include <stdexcept>
#include "chwell/event/event_bus.h"
#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/core/timer_wheel.h"

using namespace chwell;
namespace {
struct OwnedEvent : event::Event {
    std::string name() const override { return "owned"; }
    int type_id() const override { return 902; }
};
struct Reader {
    virtual ~Reader() = default;
    virtual int read() const = 0;
};
struct Unrelated { virtual ~Unrelated() = default; };
class OwnedComponent : public service::Component, public Reader {
public:
    OwnedComponent(int& calls, bool& clean, bool* fail = nullptr, bool fail_registration = false)
        : calls_(calls), clean_(clean), fail_(fail), fail_registration_(fail_registration) {}
    std::string name() const override { return "owned"; }
    int read() const override { return 42; }
    void on_register(service::Service& service) override {
        service_ = &service;
        if (!service.register_interface<Reader>(*this)) throw std::runtime_error("interface rejected");
        if (fail_registration_) {
            acquire();
            throw std::runtime_error("partial registration");
        }
    }
    bool Init() override { return acquire() && (!fail_ || !*fail_); }
    bool PreShut() override {
        const auto before = calls_;
        OwnedEvent event;
        event::EventBus::instance().publish(event);
        clean_ = before == calls_;
        return true;
    }
    bool Shut() override {
        EXPECT_EQ(service_->get_interface<Reader>(), static_cast<Reader*>(this));
        return true;
    }
private:
    bool acquire() {
        return service_->track_registration(*this,
            event::EventBus::instance().subscribe_scoped<OwnedEvent>([this](const auto&) { ++calls_; }));
    }
    int& calls_;
    bool& clean_;
    bool* fail_;
    bool fail_registration_;
    service::Service* service_ = nullptr;
};
class OwnedPlugin : public service::IPlugin {
public:
    OwnedPlugin(int& calls, bool& clean, bool* fail = nullptr) : calls_(calls), clean_(clean), fail_(fail) {}
    const std::string& GetName() const override { static const std::string name = "owned-plugin"; return name; }
    bool Install(service::Service& service) override {
        if (!service.add_component<OwnedComponent>(calls_, clean_)) return false;
        if (!service.track_registration(event::EventBus::instance().subscribe_scoped<OwnedEvent>(
                [this](const auto&) { ++calls_; }))) return false;
        return !fail_ || !*fail_;
    }
    bool Uninstall(service::Service& service) override {
        EXPECT_NE(service.get_interface<Reader>(), nullptr);
        const auto before = calls_;
        OwnedEvent event;
        event::EventBus::instance().publish(event);
        EXPECT_EQ(calls_, before);
        return true;
    }
private:
    int& calls_;
    bool& clean_;
    bool* fail_;
};
class OwnedRegistrationTest : public ::testing::Test {
protected:
    void SetUp() override { event::EventBus::instance().clear(); }
    void TearDown() override { event::EventBus::instance().clear(); }
};
}

TEST_F(OwnedRegistrationTest, StopCancelsBeforePreShutButKeepsInterfaceOfRetainedComponent) {
    int calls = 0;
    bool clean = false;
    service::Service service(0, 1);
    ASSERT_NE(service.add_component<OwnedComponent>(calls, clean), nullptr);
    ASSERT_TRUE(service.start_checked()) << service.last_error();
    OwnedEvent event;
    event::EventBus::instance().publish(event);
    EXPECT_EQ(calls, 1);
    service.stop();
    EXPECT_TRUE(clean);
    EXPECT_EQ(event::EventBus::instance().subscriber_count(), 0u);
    EXPECT_EQ(service.get_interface<Reader>()->read(), 42);
}

TEST_F(OwnedRegistrationTest, FailedInitCancelsAndRetryAcquiresFreshRegistration) {
    int calls = 0;
    bool clean = false, fail = true;
    service::Service service(0, 1);
    ASSERT_NE(service.add_component<OwnedComponent>(calls, clean, &fail), nullptr);
    EXPECT_FALSE(service.start_checked());
    EXPECT_TRUE(clean);
    EXPECT_EQ(event::EventBus::instance().subscriber_count(), 0u);
    fail = false;
    ASSERT_TRUE(service.start_checked()) << service.last_error();
    EXPECT_EQ(event::EventBus::instance().subscriber_count(), 1u);
    service.stop();
    EXPECT_EQ(event::EventBus::instance().subscriber_count(), 0u);
}

TEST_F(OwnedRegistrationTest, PluginUnloadRemovesItsInterfaceAndAllowsInstallAgain) {
    int calls = 0;
    bool clean = false;
    service::Service service(0, 1);
    auto& manager = service.plugin_manager();
    ASSERT_TRUE(manager.RegisterPlugin<OwnedPlugin>(calls, clean));
    ASSERT_TRUE(manager.InstallAll(service));
    ASSERT_NE(service.get_interface<Reader>(), nullptr);
    EXPECT_EQ(event::EventBus::instance().subscriber_count(), 1u);
    EXPECT_TRUE(manager.UninstallAll(service));
    EXPECT_EQ(service.get_interface<Reader>(), nullptr);
    EXPECT_TRUE(clean);
    EXPECT_EQ(event::EventBus::instance().subscriber_count(), 0u);
    ASSERT_TRUE(manager.InstallAll(service));
    EXPECT_NE(service.get_interface<Reader>(), nullptr);
    EXPECT_TRUE(manager.UninstallAll(service));
}

TEST_F(OwnedRegistrationTest, FailedPluginInstallCleansPartialRegistrationsAndBindings) {
    int calls = 0;
    bool clean = false, fail = true;
    service::Service service(0, 1);
    auto& manager = service.plugin_manager();
    ASSERT_TRUE(manager.RegisterPlugin<OwnedPlugin>(calls, clean, &fail));
    EXPECT_FALSE(service.start_checked());
    EXPECT_EQ(service.get_interface<Reader>(), nullptr);
    EXPECT_EQ(event::EventBus::instance().subscriber_count(), 0u);
    fail = false;
    ASSERT_TRUE(service.start_checked()) << service.last_error();
    EXPECT_EQ(event::EventBus::instance().subscriber_count(), 2u);
    service.stop();
    EXPECT_EQ(service.get_interface<Reader>(), nullptr);
    EXPECT_TRUE(clean);
}

TEST_F(OwnedRegistrationTest, ThrowingOnRegisterCleansCallbackAndInterface) {
    int calls = 0;
    bool clean = false;
    service::Service service(0, 1);
    EXPECT_THROW(service.add_component<OwnedComponent>(calls, clean, nullptr, true), std::runtime_error);
    EXPECT_TRUE(clean);
    EXPECT_EQ(service.component_count(), 0u);
    EXPECT_EQ(service.get_interface<Reader>(), nullptr);
    EXPECT_EQ(event::EventBus::instance().subscriber_count(), 0u);
}

TEST_F(OwnedRegistrationTest, ForeignIncompatibleAndDuplicateBindingsAreRejectedWithoutReplacement) {
    int calls = 0;
    bool clean = false;
    service::Service service(0, 1), foreign(0, 1);
    auto* provider = service.add_component<OwnedComponent>(calls, clean);
    ASSERT_NE(provider, nullptr);
    EXPECT_FALSE(foreign.register_interface<Reader>(*provider));
    EXPECT_FALSE(service.register_interface<Unrelated>(*provider));
    EXPECT_FALSE(service.register_interface<Reader>(*provider));
    EXPECT_EQ(service.get_interface<Reader>(), static_cast<Reader*>(provider));
    EXPECT_EQ(service.get_component<OwnedComponent>(), provider);
    EXPECT_EQ(service.get_interface<OwnedComponent>(), nullptr);
}

TEST_F(OwnedRegistrationTest, RejectedTrackingCancelsNewTokenImmediately) {
    int calls = 0;
    bool clean = false;
    service::Service service(0, 1), foreign(0, 1);
    auto* provider = service.add_component<OwnedComponent>(calls, clean);
    ASSERT_NE(provider, nullptr);
    auto& bus = event::EventBus::instance();
    EXPECT_FALSE(foreign.track_registration(*provider, bus.subscribe_scoped<OwnedEvent>([](const auto&) {})));
    EXPECT_FALSE(service.track_registration(bus.subscribe_scoped<OwnedEvent>([](const auto&) {})));
    EXPECT_EQ(bus.subscriber_count(), 0u);
    ASSERT_TRUE(service.start_checked());
    service.stop();
    EXPECT_FALSE(service.track_registration(*provider, bus.subscribe_scoped<OwnedEvent>([](const auto&) {})));
    EXPECT_EQ(bus.subscriber_count(), 0u);
}

TEST_F(OwnedRegistrationTest, DestructorCancelsResourcesOfNeverInitializedComponent) {
    int calls = 0;
    bool clean = false;
    {
        service::Service service(0, 1);
        auto* provider = service.add_component<OwnedComponent>(calls, clean);
        ASSERT_NE(provider, nullptr);
        ASSERT_TRUE(service.track_registration(*provider,
            event::EventBus::instance().subscribe_scoped<OwnedEvent>([&](const auto&) { ++calls; })));
    }
    EXPECT_TRUE(clean);
    EXPECT_EQ(event::EventBus::instance().subscriber_count(), 0u);
}

TEST_F(OwnedRegistrationTest, IgnoredOnRegisterCollisionIsReportedAndCleaned) {
    class IgnoringCollision : public service::Component, public Reader {
    public:
        std::string name() const override { return "collision"; }
        int read() const override { return 0; }
        void on_register(service::Service& service) override { service.register_interface<Reader>(*this); }
    };
    int calls = 0;
    bool clean = false;
    service::Service service(0, 1);
    auto* first = service.add_component<OwnedComponent>(calls, clean);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(service.add_component<IgnoringCollision>(), nullptr);
    EXPECT_EQ(service.component_count(), 1u);
    EXPECT_EQ(service.get_interface<Reader>(), static_cast<Reader*>(first));
    EXPECT_NE(service.last_error().find("duplicate interface"), std::string::npos);
}

TEST_F(OwnedRegistrationTest, ServiceCleansProtocolAndTimerRegistrationsTogether) {
    int calls = 0;
    bool clean = false;
    core::TimerWheel wheel(1000, 60, 1);
    service::Service service(0, 1);
    auto* router = service.add_component<service::ProtocolRouterComponent>();
    auto* owner = service.add_component<OwnedComponent>(calls, clean);
    ASSERT_NE(owner, nullptr);
    ASSERT_TRUE(service.track_registration(*owner, router->register_handler_scoped(77,
        [&](const auto&, const auto&) { ++calls; })));
    ASSERT_TRUE(service.track_registration(*owner, wheel.add_repeat_timer_scoped(1000, [&] { ++calls; })));
    auto conn = std::make_shared<net::TcpConnection>(net::TcpSocket());
    auto frame = protocol::serialize(protocol::Message(77, "owned"));
    ASSERT_TRUE(service.start_checked());
    router->on_message(conn, std::string_view(frame.data(), frame.size()));
    EXPECT_EQ(calls, 1);
    service.stop();
    router->on_message(conn, std::string_view(frame.data(), frame.size()));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(wheel.get_next_expire_time(), -1);
    EXPECT_TRUE(clean);
}

TEST_F(OwnedRegistrationTest, ServiceCancelsInReverseOwnerAndWithinOwnerOrder) {
    int calls = 0;
    bool clean = false;
    std::vector<int> cancelled;
    service::Service service(0, 1);
    auto* first = service.add_component<OwnedComponent>(calls, clean);
    auto* second = service.add_component<service::ProtocolRouterComponent>();
    ASSERT_TRUE(service.track_registration(*first, core::Registration([&] { cancelled.push_back(1); })));
    ASSERT_TRUE(service.track_registration(*second, core::Registration([&] { cancelled.push_back(2); })));
    ASSERT_TRUE(service.track_registration(*first, core::Registration([&] { cancelled.push_back(3); })));
    service.stop();
    EXPECT_EQ(cancelled, (std::vector<int>{2, 3, 1}));
}
