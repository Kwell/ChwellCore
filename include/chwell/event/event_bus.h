#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <any>
#include <typeindex>

#include "chwell/core/logger.h"

namespace chwell {
namespace event {

// 事件基类
class Event {
public:
    virtual ~Event() = default;
    virtual std::string name() const = 0;
    virtual int type_id() const = 0;
    
    int64_t timestamp() const { return timestamp_; }
    void set_timestamp(int64_t ts) { timestamp_ = ts; }
    
    bool cancelled() const { return cancelled_; }
    void cancel() { cancelled_ = true; }
    
protected:
    int64_t timestamp_ = 0;
    bool cancelled_ = false;
};

// 事件处理器 ID 类型
using HandlerId = uint64_t;

// 事件处理器基类
class EventHandlerBase {
public:
    virtual ~EventHandlerBase() = default;
    virtual void invoke(const Event& event) = 0;
    virtual std::type_index event_type() const = 0;
};

// 类型安全的处理器
template<typename EventT>
class EventHandler : public EventHandlerBase {
public:
    using Callback = std::function<void(const EventT&)>;
    
    explicit EventHandler(Callback callback) : callback_(std::move(callback)) {}
    
    void invoke(const Event& event) override {
        if (callback_) {
            callback_(static_cast<const EventT&>(event));
        }
    }
    
    std::type_index event_type() const override {
        return std::type_index(typeid(EventT));
    }
    
private:
    Callback callback_;
};

// 事件订阅者
struct Subscriber {
    HandlerId id;
    int priority;  // 优先级，越大越先执行
    std::unique_ptr<EventHandlerBase> handler;
    
    Subscriber(HandlerId id, int priority, std::unique_ptr<EventHandlerBase> h)
        : id(id), priority(priority), handler(std::move(h)) {}
};

// 事件总线
class EventBus {
public:
    static EventBus& instance() {
        static EventBus inst;
        return inst;
    }
    
    // 订阅事件
    template<typename EventT>
    HandlerId subscribe(std::function<void(const EventT&)> callback, int priority = 0) {
        static_assert(std::is_base_of<Event, EventT>::value, "EventT must derive from Event");
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        HandlerId id = next_id_++;
        auto handler = std::make_unique<EventHandler<EventT>>(std::move(callback));
        auto subscriber = std::make_unique<Subscriber>(id, priority, std::move(handler));
        
        auto& subs = subscribers_[std::type_index(typeid(EventT))];
        subs.push_back(std::move(subscriber));
        
        // 按优先级排序
        std::sort(subs.begin(), subs.end(), 
                  [](const std::unique_ptr<Subscriber>& a, const std::unique_ptr<Subscriber>& b) {
                      return a->priority > b->priority;
                  });
        
        return id;
    }
    
    // 取消订阅
    void unsubscribe(HandlerId id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        for (auto& pair : subscribers_) {
            auto& subs = pair.second;
            auto it = std::find_if(subs.begin(), subs.end(),
                                   [id](const std::unique_ptr<Subscriber>& s) {
                                       return s->id == id;
                                   });
            if (it != subs.end()) {
                subs.erase(it);
                return;
            }
        }
    }
    
    // 发布事件（同步）
    template<typename EventT>
    void publish(EventT& event) {
        static_assert(std::is_base_of<Event, EventT>::value, "EventT must derive from Event");
        
        // 设置时间戳
        if (event.timestamp() == 0) {
            event.set_timestamp(current_time_ms());
        }
        
        std::vector<std::unique_ptr<EventHandlerBase>*> handlers;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = subscribers_.find(std::type_index(typeid(EventT)));
            if (it != subscribers_.end()) {
                for (const auto& sub : it->second) {
                    handlers.push_back(&sub->handler);
                }
            }
        }
        
        // 在锁外执行回调
        for (auto* handler : handlers) {
            if (event.cancelled()) break;
            try {
                (*handler)->invoke(event);
            } catch (const std::exception& e) {
                CHWELL_LOG_ERROR("Event handler exception: " << e.what());
            }
        }
    }
    
    // 发布事件（移动语义）
    template<typename EventT>
    void publish(EventT&& event) {
        EventT e = std::forward<EventT>(event);
        publish(e);
    }
    
    // 清除所有订阅
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers_.clear();
    }
    
    // 获取订阅数量
    size_t subscriber_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& pair : subscribers_) {
            count += pair.second.size();
        }
        return count;
    }
    
private:
    EventBus() : next_id_(1) {}
    
    static int64_t current_time_ms() {
        auto now = std::chrono::steady_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    }
    
    mutable std::mutex mutex_;
    std::unordered_map<std::type_index, std::vector<std::unique_ptr<Subscriber>>> subscribers_;
    HandlerId next_id_;
};

// 常用事件定义

// 连接事件
class ConnectionEvent : public Event {
public:
    enum class Type { CONNECT, DISCONNECT };
    
    ConnectionEvent(Type t, uint64_t conn_id, const std::string& addr = "")
        : type_(t), connection_id_(conn_id), address_(addr) {}
    
    std::string name() const override {
        return type_ == Type::CONNECT ? "ConnectionEvent:Connect" : "ConnectionEvent:Disconnect";
    }
    
    int type_id() const override { return 1; }
    
    Type type() const { return type_; }
    uint64_t connection_id() const { return connection_id_; }
    const std::string& address() const { return address_; }
    
private:
    Type type_;
    uint64_t connection_id_;
    std::string address_;
};

// 登录事件
class LoginEvent : public Event {
public:
    LoginEvent(uint64_t conn_id, const std::string& player_id, bool success = true)
        : connection_id_(conn_id), player_id_(player_id), success_(success) {}
    
    std::string name() const override { return "LoginEvent"; }
    int type_id() const override { return 2; }
    
    uint64_t connection_id() const { return connection_id_; }
    const std::string& player_id() const { return player_id_; }
    bool success() const { return success_; }
    
private:
    uint64_t connection_id_;
    std::string player_id_;
    bool success_;
};

// 聊天事件
class ChatEvent : public Event {
public:
    ChatEvent(const std::string& sender, const std::string& channel, const std::string& message)
        : sender_(sender), channel_(channel), message_(message) {}
    
    std::string name() const override { return "ChatEvent"; }
    int type_id() const override { return 3; }
    
    const std::string& sender() const { return sender_; }
    const std::string& channel() const { return channel_; }
    const std::string& message() const { return message_; }
    
private:
    std::string sender_;
    std::string channel_;
    std::string message_;
};

// 游戏事件基类
class GameEvent : public Event {
public:
    GameEvent(uint64_t entity_id = 0) : entity_id_(entity_id) {}
    
    uint64_t entity_id() const { return entity_id_; }
    
protected:
    uint64_t entity_id_;
};

// 实体创建事件
class EntityCreateEvent : public GameEvent {
public:
    EntityCreateEvent(uint64_t entity_id, int x, int y, int type)
        : GameEvent(entity_id), x_(x), y_(y), entity_type_(type) {}
    
    std::string name() const override { return "EntityCreateEvent"; }
    int type_id() const override { return 10; }
    
    int x() const { return x_; }
    int y() const { return y_; }
    int entity_type() const { return entity_type_; }
    
private:
    int x_, y_, entity_type_;
};

// 实体移动事件
class EntityMoveEvent : public GameEvent {
public:
    EntityMoveEvent(uint64_t entity_id, int old_x, int old_y, int new_x, int new_y)
        : GameEvent(entity_id), old_x_(old_x), old_y_(old_y), new_x_(new_x), new_y_(new_y) {}
    
    std::string name() const override { return "EntityMoveEvent"; }
    int type_id() const override { return 11; }
    
    int old_x() const { return old_x_; }
    int old_y() const { return old_y_; }
    int new_x() const { return new_x_; }
    int new_y() const { return new_y_; }
    
private:
    int old_x_, old_y_, new_x_, new_y_;
};

// 实体销毁事件
class EntityDestroyEvent : public GameEvent {
public:
    explicit EntityDestroyEvent(uint64_t entity_id) : GameEvent(entity_id) {}
    
    std::string name() const override { return "EntityDestroyEvent"; }
    int type_id() const override { return 12; }
};

// 事件总线辅助宏
#define EVENT_SUBSCRIBE(EventT, callback, priority) \
    chwell::event::EventBus::instance().subscribe<EventT>(callback, priority)

#define EVENT_UNSUBSCRIBE(handler_id) \
    chwell::event::EventBus::instance().unsubscribe(handler_id)

#define EVENT_PUBLISH(event) \
    chwell::event::EventBus::instance().publish(event)

} // namespace event
} // namespace chwell