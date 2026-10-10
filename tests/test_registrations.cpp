#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <thread>
#include <stdexcept>
#include "chwell/core/registration.h"
#include "chwell/core/timer_wheel.h"
#include "chwell/event/event_bus.h"
#include "chwell/service/interface_registry.h"

using namespace chwell;
using namespace std::chrono_literals;

namespace {
struct RegistrationEvent : event::Event {
    std::string name() const override { return "registration"; }
    int type_id() const override { return 901; }
};
class ScopedEventTest : public ::testing::Test {
protected:
    void SetUp() override { event::EventBus::instance().clear(); }
    void TearDown() override { event::EventBus::instance().clear(); }
};
}

TEST(RegistrationTest, MoveAndResetCancelExactlyOnce) {
    int first = 0, second = 0;
    core::Registration a([&] { ++first; });
    core::Registration b([&] { ++second; });
    b = std::move(a);
    EXPECT_EQ(second, 1);
    EXPECT_FALSE(a);
    EXPECT_TRUE(b);
    b = std::move(b); // Self-move must not cancel.
    EXPECT_EQ(first, 0);
    b.reset();
    b.reset();
    EXPECT_EQ(first, 1);
}

TEST(RegistrationTest, GroupContinuesReverseCleanupAfterThrowAndRejectsReentrantAdd) {
    std::vector<int> order;
    core::RegistrationGroup group;
    ASSERT_TRUE(group.add(core::Registration([&] { order.push_back(1); })));
    ASSERT_TRUE(group.add(core::Registration([&] { order.push_back(2); throw std::runtime_error("cleanup"); })));
    ASSERT_TRUE(group.add(core::Registration([&] {
        order.push_back(3);
        EXPECT_FALSE(group.add(core::Registration([&] { order.push_back(4); })));
        group.clear();
    })));
    group.clear();
    group.clear();
    EXPECT_EQ(order, (std::vector<int>{3, 4, 2, 1}));
}

TEST(RegistrationTest, CancelledSlotSuppressesCopiedDispatchAndReleasesCapturedObject) {
    auto object = std::make_shared<int>(0);
    std::weak_ptr<int> weak = object;
    auto slot = std::make_shared<core::detail::CallbackSlot<>>([object] { ++*object; });
    auto snapshot = slot->wrapper(slot);
    object.reset();
    snapshot();
    EXPECT_EQ(*weak.lock(), 1);
    slot->cancel();
    EXPECT_TRUE(weak.expired());
    snapshot();
    slot.reset();
    snapshot();
}

TEST(RegistrationTest, SlotPreservesMutableCallbackStateAcrossInvocations) {
    int observed = 0;
    auto slot = std::make_shared<core::detail::CallbackSlot<>>(
        [count = 0, &observed]() mutable { observed = ++count; });
    auto snapshot = slot->wrapper(slot);
    snapshot();
    snapshot();
    EXPECT_EQ(observed, 2);
    slot->cancel();
    snapshot();
    EXPECT_EQ(observed, 2);
}

TEST_F(ScopedEventTest, DestructionUnsubscribesAndExplicitMoveKeepsSubscription) {
    auto& bus = event::EventBus::instance();
    int calls = 0;
    RegistrationEvent event;
    {
        auto token = bus.subscribe_scoped<RegistrationEvent>([&](const auto&) { ++calls; });
        auto moved = std::move(token);
        EXPECT_FALSE(token);
        bus.publish(event);
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(bus.subscriber_count(), 1u);
    }
    EXPECT_EQ(bus.subscriber_count(), 0u);
    bus.publish(event);
    EXPECT_EQ(calls, 1);
}

TEST_F(ScopedEventTest, EarlierHandlerCancelsLaterHandlerAlreadyInPublishSnapshot) {
    auto& bus = event::EventBus::instance();
    int calls = 0;
    auto later = bus.subscribe_scoped<RegistrationEvent>([&](const auto&) { ++calls; });
    auto first = bus.subscribe_scoped<RegistrationEvent>([&](const auto&) { later.reset(); }, 100);
    RegistrationEvent event;
    bus.publish(event);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(bus.subscriber_count(), 1u);
}

TEST_F(ScopedEventTest, SelfCancellationAllowsNestedPublicationWithoutDeadlock) {
    auto& bus = event::EventBus::instance();
    int calls = 0;
    core::Registration token;
    token = bus.subscribe_scoped<RegistrationEvent>([&](const auto&) {
        ++calls;
        token.reset();
        RegistrationEvent nested;
        bus.publish(nested);
    });
    RegistrationEvent event;
    bus.publish(event);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(bus.subscriber_count(), 0u);
}

TEST_F(ScopedEventTest, CancellationWaitsForInFlightHandlerOnAnotherThread) {
    auto& bus = event::EventBus::instance();
    std::promise<void> entered, release, cancelling;
    auto released = release.get_future();
    auto token = bus.subscribe_scoped<RegistrationEvent>([&](const auto&) {
        entered.set_value();
        released.wait_for(3s);
    });
    auto publishing = std::async(std::launch::async, [&] { RegistrationEvent event; bus.publish(event); });
    const auto started = entered.get_future().wait_for(2s);
    EXPECT_EQ(started, std::future_status::ready);
    auto resetting = std::async(std::launch::async, [&] { cancelling.set_value(); token.reset(); });
    cancelling.get_future().wait();
    EXPECT_EQ(resetting.wait_for(30ms), std::future_status::timeout);
    release.set_value();
    publishing.get();
    resetting.get();
    EXPECT_EQ(bus.subscriber_count(), 0u);
}

TEST_F(ScopedEventTest, EmptyCallbackReturnsNoRegistration) {
    EXPECT_FALSE(event::EventBus::instance().subscribe_scoped<RegistrationEvent>({}));
    EXPECT_EQ(event::EventBus::instance().subscriber_count(), 0u);
}

TEST(ScopedTimerTest, CancellationSkipsTaskAlreadyExtractedWithOtherDueTasks) {
    core::TimerWheel wheel(1000, 60, 1);
    int calls = 0;
    core::Registration second;
    auto first = wheel.add_timer_scoped(1, [&] { second.reset(); });
    second = wheel.add_timer_scoped(1, [&] { ++calls; });
    std::this_thread::sleep_for(10ms);
    wheel.tick();
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(wheel.get_next_expire_time(), -1);
}

TEST(ScopedTimerTest, SelfCancellationNeverRearmsRepeatingTask) {
    core::TimerWheel wheel(1000, 60, 1);
    int calls = 0;
    core::Registration token;
    token = wheel.add_repeat_timer_scoped(1, [&] { ++calls; token.reset(); });
    std::this_thread::sleep_for(10ms);
    wheel.tick();
    for (int i = 0; i < 60; ++i) wheel.tick();
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(token);
    EXPECT_EQ(wheel.get_next_expire_time(), -1);
}

TEST(ScopedTimerTest, TokenCanOutliveSourceAndCannotCancelNewWheelTimer) {
    int calls = 0;
    core::Registration token;
    {
        core::TimerWheel old(1000, 60, 1);
        token = old.add_timer_scoped(1, [&] { calls += 100; });
    }
    core::TimerWheel fresh(1000, 60, 1);
    auto fresh_token = fresh.add_timer_scoped(1, [&] { ++calls; });
    token.reset();
    std::this_thread::sleep_for(10ms);
    fresh.tick();
    EXPECT_EQ(calls, 1);
}

TEST(ScopedTimerTest, CancellationWaitsForExecutingTimerBeforeReturning) {
    core::TimerWheel wheel(1, 60, 1);
    std::promise<void> entered, release, cancelling;
    auto released = release.get_future();
    auto token = wheel.add_repeat_timer_scoped(2, [&] {
        entered.set_value();
        released.wait_for(3s);
    });
    wheel.start();
    EXPECT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    auto resetting = std::async(std::launch::async, [&] { cancelling.set_value(); token.reset(); });
    cancelling.get_future().wait();
    EXPECT_EQ(resetting.wait_for(30ms), std::future_status::timeout);
    release.set_value();
    resetting.get();
    wheel.stop();
    EXPECT_EQ(wheel.get_next_expire_time(), -1);
}

TEST(ScopedTimerTest, InvalidInputsAcquireNothing) {
    core::TimerWheel wheel;
    EXPECT_FALSE(wheel.add_timer_scoped(0, [] {}));
    EXPECT_FALSE(wheel.add_repeat_timer_scoped(-1, [] {}));
    EXPECT_FALSE(wheel.add_timer_scoped(10, {}));
    EXPECT_EQ(wheel.get_next_expire_time(), -1);
}

TEST(InterfaceRegistryTest, ExactBindingsAdjustMultipleInheritancePointersAndRemoveOnlyOwner) {
    struct Left { virtual ~Left() = default; virtual int left() const = 0; };
    struct Right { virtual ~Right() = default; virtual int right() const = 0; };
    struct Provider : Left, Right { int left() const override { return 1; } int right() const override { return 2; } };
    Provider first, second;
    service::InterfaceRegistry registry;
    EXPECT_TRUE(registry.add<Left>(&first, &first));
    EXPECT_TRUE(registry.add<Right>(&second, &second));
    EXPECT_FALSE(registry.add<Left>(&second, &second));
    EXPECT_EQ(registry.get<Provider>(), nullptr); // No implicit concrete-type scan.
    EXPECT_EQ(registry.get<Left>()->left(), 1);
    EXPECT_EQ(registry.get<Right>(), static_cast<Right*>(&second));
    registry.remove_owner(&first);
    EXPECT_EQ(registry.get<Left>(), nullptr);
    EXPECT_NE(registry.get<Right>(), nullptr);
    EXPECT_TRUE(registry.add<Left>(&second, &second));
    registry.remove_owner(&second);
    EXPECT_EQ(registry.get<Left>(), nullptr);
    EXPECT_EQ(registry.get<Right>(), nullptr);
    EXPECT_FALSE(registry.add<Left>(nullptr, &first));
    EXPECT_FALSE(registry.add<Left>(&first, nullptr));
}
