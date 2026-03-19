#pragma once

#include <string>
#include <vector>
#include <memory>
#include <type_traits>

namespace chwell {
namespace net {

// 前向声明
class TcpConnection;
class WsConnection;

typedef std::shared_ptr<TcpConnection> TcpConnectionPtr;
typedef std::shared_ptr<WsConnection> WsConnectionPtr;

// 连接基类：提供所有连接类型的公共接口
class IConnection {
public:
    virtual ~IConnection() = default;

    // 获取原生句柄
    virtual int native_handle() const = 0;

    // 发送数据
    virtual void send(const std::vector<char>& data) = 0;
    virtual void send_text(const std::string& text) = 0;

    // 关闭连接
    virtual void close() = 0;

    // 获取连接类型
    virtual std::string type() const = 0;
};

// TcpConnection 适配器
class TcpConnectionAdapter : public IConnection {
public:
    explicit TcpConnectionAdapter(TcpConnectionPtr conn) : conn_(conn) {}

    int native_handle() const override {
        return conn_->native_handle();
    }

    void send(const std::vector<char>& data) override {
        conn_->send(data);
    }

    void send_text(const std::string& text) override {
        std::vector<char> data(text.begin(), text.end());
        conn_->send(data);
    }

    void close() override {
        conn_->close();
    }

    std::string type() const override {
        return "tcp";
    }

private:
    TcpConnectionPtr conn_;
};

// WsConnection 适配器
class WsConnectionAdapter : public IConnection {
public:
    explicit WsConnectionAdapter(WsConnectionPtr conn) : conn_(conn) {}

    int native_handle() const override {
        return conn_->native_handle();
    }

    void send(const std::vector<char>& data) override {
        // WebSocket 二进制帧
        // 需要实现 WebSocket 帧格式
        // 暂时转换为文本发送
        std::string text(data.begin(), data.end());
        conn_->send_text(text);
    }

    void send_text(const std::string& text) override {
        conn_->send_text(text);
    }

    void close() override {
        conn_->close();
    }

    std::string type() const override {
        return "ws";
    }

private:
    WsConnectionPtr conn_;
};

// 通用连接指针类型
typedef std::shared_ptr<IConnection> ConnectionPtr;

// 工厂函数：从 TcpConnection 创建 IConnection
inline ConnectionPtr make_connection(TcpConnectionPtr conn) {
    return std::make_shared<TcpConnectionAdapter>(conn);
}

// 工厂函数：从 WsConnection 创建 IConnection
inline ConnectionPtr make_connection(WsConnectionPtr conn) {
    return std::make_shared<WsConnectionAdapter>(conn);
}

} // namespace net
} // namespace chwell
