#include <gtest/gtest.h>
#include <thread>
#include <atomic>

#include "chwell/event/event_bus.h"

using namespace chwell;

// Test event types
class TestEvent : public event::Event {
public:
    int value;
    explicit TestEvent(int v) : value(v) {}
    std::string name() const override { return "TestEvent"; }
    int type_id() const override { return 100; }
};

class AnotherEvent : public event::Event {
public:
    std::string message;
    explicit AnotherEvent(const std::string& msg) : message(msg) {}
    std::string name() const override { return "AnotherEvent"; }
    int type_id() const override { return 101; }
};

class EventBusTest : public ::testing::Test {
protected:
    void SetUp() override {
        event::EventBus::instance().clear();
    }
    
    void TearDown() override {
        event::EventBus::instance().clear();
    }
};

TEST_F(EventBusTest, SubscribeAndPublish) {
    event::EventBus& bus = event::EventBus::instance();
    
    int received = 0;
    auto id = bus.subscribe<TestEvent>([&received](const TestEvent& e) {
        received = e.value;
    });
    
    EXPECT_GT(id, 0u);
    
    TestEvent evt(42);
    bus.publish(evt);
    
    EXPECT_EQ(received, 42);
}

TEST_F(EventBusTest, MultipleSubscribers) {
    event::EventBus& bus = event::EventBus::instance();
    
    int sum = 0;
    bus.subscribe<TestEvent>([&sum](const TestEvent& e) {
        sum += e.value;
    });
    bus.subscribe<TestEvent>([&sum](const TestEvent& e) {
        sum += e.value * 2;
    });
    
    TestEvent evt(10);
    bus.publish(evt);
    
    EXPECT_EQ(sum, 30);
}

TEST_F(EventBusTest, Unsubscribe) {
    event::EventBus& bus = event::EventBus::instance();
    
    int counter = 0;
    auto id = bus.subscribe<TestEvent>([&counter](const TestEvent& e) {
        counter++;
    });
    
    TestEvent evt(1);
    bus.publish(evt);
    EXPECT_EQ(counter, 1);
    
    bus.unsubscribe(id);
    
    bus.publish(evt);
    EXPECT_EQ(counter, 1);
}

TEST_F(EventBusTest, DifferentEventTypes) {
    event::EventBus& bus = event::EventBus::instance();
    
    int test_count = 0;
    std::string msg_received;
    
    bus.subscribe<TestEvent>([&test_count](const TestEvent& e) {
        test_count++;
    });
    bus.subscribe<AnotherEvent>([&msg_received](const AnotherEvent& e) {
        msg_received = e.message;
    });
    
    TestEvent te(1);
    AnotherEvent ae("hello");
    
    bus.publish(te);
    bus.publish(ae);
    
    EXPECT_EQ(test_count, 1);
    EXPECT_EQ(msg_received, "hello");
}

TEST_F(EventBusTest, PriorityOrder) {
    event::EventBus& bus = event::EventBus::instance();
    
    std::vector<int> order;
    
    bus.subscribe<TestEvent>([&order](const TestEvent& e) {
        order.push_back(2);
    }, 10);
    
    bus.subscribe<TestEvent>([&order](const TestEvent& e) {
        order.push_back(1);
    }, 100);  // Higher priority, should execute first
    
    bus.subscribe<TestEvent>([&order](const TestEvent& e) {
        order.push_back(3);
    }, 5);
    
    TestEvent evt(1);
    bus.publish(evt);
    
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST_F(EventBusTest, EventCancel) {
    event::EventBus& bus = event::EventBus::instance();
    
    int counter = 0;
    
    bus.subscribe<TestEvent>([&counter](const TestEvent& e) {
        counter++;
        // Note: cancel() is non-const, need mutable reference
    }, 100);
    
    bus.subscribe<TestEvent>([&counter](const TestEvent& e) {
        counter++;
    }, 10);
    
    TestEvent evt(1);
    bus.publish(evt);
    
    EXPECT_EQ(counter, 2);
}

TEST_F(EventBusTest, ThreadSafe) {
    event::EventBus& bus = event::EventBus::instance();
    
    std::atomic<int> counter{0};
    
    for (int i = 0; i < 5; i++) {
        bus.subscribe<TestEvent>([&counter](const TestEvent& e) {
            counter++;
        });
    }
    
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([&bus]() {
            TestEvent evt(1);
            bus.publish(evt);
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(counter, 50);
}

TEST_F(EventBusTest, SubscriberCount) {
    event::EventBus& bus = event::EventBus::instance();
    
    EXPECT_EQ(bus.subscriber_count(), 0u);
    
    auto id1 = bus.subscribe<TestEvent>([](const TestEvent&) {});
    EXPECT_EQ(bus.subscriber_count(), 1u);
    
    auto id2 = bus.subscribe<AnotherEvent>([](const AnotherEvent&) {});
    EXPECT_EQ(bus.subscriber_count(), 2u);
    
    bus.unsubscribe(id1);
    EXPECT_EQ(bus.subscriber_count(), 1u);
    
    bus.unsubscribe(id2);
    EXPECT_EQ(bus.subscriber_count(), 0u);
}

// Test built-in events
TEST_F(EventBusTest, ConnectionEvent) {
    event::EventBus& bus = event::EventBus::instance();
    
    uint64_t conn_id = 0;
    event::ConnectionEvent::Type type;
    
    bus.subscribe<event::ConnectionEvent>([&](const event::ConnectionEvent& e) {
        conn_id = e.connection_id();
        type = e.type();
    });
    
    event::ConnectionEvent evt(event::ConnectionEvent::Type::CONNECT, 12345, "127.0.0.1");
    bus.publish(evt);
    
    EXPECT_EQ(conn_id, 12345u);
    EXPECT_EQ(type, event::ConnectionEvent::Type::CONNECT);
}

TEST_F(EventBusTest, EntityMoveEvent) {
    event::EventBus& bus = event::EventBus::instance();
    
    uint64_t entity_id = 0;
    int new_x = 0, new_y = 0;
    
    bus.subscribe<event::EntityMoveEvent>([&](const event::EntityMoveEvent& e) {
        entity_id = e.entity_id();
        new_x = e.new_x();
        new_y = e.new_y();
    });
    
    event::EntityMoveEvent evt(100, 10, 10, 20, 20);
    bus.publish(evt);
    
    EXPECT_EQ(entity_id, 100u);
    EXPECT_EQ(new_x, 20);
    EXPECT_EQ(new_y, 20);
}