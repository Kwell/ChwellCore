#pragma once

#include <shared_mutex>
#include <cstdint>
#include <string_view>
#include <functional>
#include <unordered_map>
#include <vector>
#include <atomic>
#include "chwell/service/component.h"
#include "chwell/core/registration.h"
#include "chwell/protocol/message.h"
#include "chwell/protocol/parser.h"

namespace chwell {
namespace net {
class TcpConnection;
typedef std::shared_ptr<TcpConnection> TcpConnectionPtr;
} // namespace net

namespace service {

// 协议路由组件：负责解析协议并按 cmd 路由到不同的处理器
// 使用方式：
//   1. 注册 ProtocolRouterComponent 到 Service
//   2. 调用 register_handler(cmd, handler) 注册各个 cmd 的处理器
//   3. 当收到消息时，会自动解析协议并按 cmd 路由
class ProtocolRouterComponent : public Component {
public:
    typedef std::function<void(const net::TcpConnectionPtr&, const protocol::Message&)> MessageHandler;

    ProtocolRouterComponent() {}
    ~ProtocolRouterComponent() override { registration_source_->detach(); }

    virtual std::string name() const override {
        return "ProtocolRouterComponent";
    }

    // 注册一个 cmd 的处理器
    void register_handler(std::uint16_t cmd, MessageHandler handler) {
        std::unique_lock lock(handlers_mutex_);
        handlers_[cmd] = {next_handler_id_++, std::move(handler)};
    }

    // Scoped registration rejects collisions; cancelling an old token never removes
    // a later handler installed by the legacy replacement API.
    core::Registration register_handler_scoped(std::uint16_t cmd, MessageHandler handler) {
        if (!handler) return {};
        auto slot = std::make_shared<core::detail::CallbackSlot<const net::TcpConnectionPtr&,
                                                              const protocol::Message&>>(std::move(handler));
        std::uint64_t id;
        {
            std::unique_lock lock(handlers_mutex_);
            if (handlers_.count(cmd)) return {};
            id = next_handler_id_++;
            handlers_.emplace(cmd, Handler{id, slot->wrapper(slot)});
        }
        std::weak_ptr<core::detail::RegistrationSource<ProtocolRouterComponent>> source = registration_source_;
        try {
            return core::Registration([slot, source, cmd, id] {
                slot->cancel();
                if (auto owner = source.lock()) {
                    std::lock_guard<std::mutex> lock(owner->mutex);
                    if (owner->source) owner->source->remove_handler(cmd, id);
                }
            });
        } catch (...) { slot->cancel(); remove_handler(cmd, id); throw; }
    }

    // 组件接口：收到原始消息时，解析协议并路由
    virtual void on_message(const net::TcpConnectionPtr& conn,
                            std::string_view data) override;

    // 组件接口：连接断开时清理解析器
    virtual void on_disconnect(const net::TcpConnectionPtr& conn) override;

    // 发送协议消息的辅助函数
    static void send_message(const net::TcpConnectionPtr& conn, const protocol::Message& msg);

private:
    void remove_handler(std::uint16_t cmd, std::uint64_t id) {
        std::unique_lock lock(handlers_mutex_);
        auto it = handlers_.find(cmd);
        if (it != handlers_.end() && it->second.id == id) handlers_.erase(it);
    }
    // 为每个连接维护一个解析器（处理粘包/拆包）
    // 使用 uint64_t 连接 ID 作为 key 而非裸指针，避免指针地址复用导致
    // 新连接错误继承旧连接的解析器状态（半包残留等）
    std::unordered_map<uint64_t, protocol::Parser> parsers_;
    // 连接指针 -> 连接 ID 的映射（用于 on_disconnect 时查找）
    std::unordered_map<std::uint64_t, uint64_t> conn_ids_;
    std::atomic<uint64_t> next_conn_id_{1};
    mutable std::shared_mutex parsers_mutex_;

    struct Handler { std::uint64_t id; MessageHandler callback; };
    std::unordered_map<std::uint16_t, Handler> handlers_;
    std::uint64_t next_handler_id_ = 1;
    mutable std::shared_mutex handlers_mutex_;
    std::shared_ptr<core::detail::RegistrationSource<ProtocolRouterComponent>> registration_source_ =
        std::make_shared<core::detail::RegistrationSource<ProtocolRouterComponent>>(this);
};

} // namespace service
} // namespace chwell
