#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <functional>

namespace chwell {
namespace net {

// 通用连接接口：为 TCP / UDP / WS 等多种传输提供统一抽象
// 注意：此接口不同于 connection_adapter.h 中的 IConnection，以避免 ODR 冲突
class INetConnection {
public:
    virtual ~INetConnection() {}

    // 发送原始字节数据
    virtual void send(const std::vector<char>& data) = 0;

    // 关闭连接
    virtual void close() = 0;
};

typedef std::shared_ptr<INetConnection> INetConnectionPtr;
typedef std::function<void(const INetConnectionPtr&, const std::vector<char>&)> INetMessageCallback;
typedef std::function<void(const INetConnectionPtr&)> INetConnectionCallback;

// 通用服务器接口：可以由 TCP/WS 等实现
class IServer {
public:
    virtual ~IServer() {}

    // 启动监听/接受连接
    virtual void start() = 0;

    // 停止服务器（可选实现）
    virtual void stop() = 0;

    virtual void set_message_callback(const INetMessageCallback& cb) = 0;
    virtual void set_connection_callback(const INetConnectionCallback& cb) = 0;
    virtual void set_disconnect_callback(const INetConnectionCallback& cb) = 0;
};

} // namespace net
} // namespace chwell

