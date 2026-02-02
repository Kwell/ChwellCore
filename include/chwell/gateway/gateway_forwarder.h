#pragma once

#include <unordered_map>
#include <string>
#include <memory>
#include <mutex>

#include "chwell/service/component.h"
#include "chwell/protocol/message.h"

namespace chwell {
namespace net {
class TcpConnection;
typedef std::shared_ptr<TcpConnection> TcpConnectionPtr;
}  // namespace net

namespace gateway {

// GatewayForwarderComponent：网关转发组件
// 负责维护客户端与后端逻辑服的连接映射，将需要转发的消息发送到后端并回传响应
class GatewayForwarderComponent : public service::Component {
public:
    GatewayForwarderComponent(const std::string& backend_host,
                              unsigned short backend_port);

    virtual std::string name() const override {
        return "GatewayForwarderComponent";
    }

    virtual void on_register(service::Service& svc) override;
    virtual void on_disconnect(const net::TcpConnectionPtr& conn) override;

    // 将消息转发到后端，响应会通过 ProtocolRouterComponent 回传给客户端
    void forward(const net::TcpConnectionPtr& client_conn,
                 const protocol::Message& msg);

    // 检查是否已建立后端连接
    bool has_backend(const net::TcpConnectionPtr& client_conn) const;

private:
    net::TcpConnectionPtr connect_backend(const net::TcpConnectionPtr& client_conn);
    void on_backend_message(const net::TcpConnectionPtr& backend_conn,
                            const std::vector<char>& data);
    void on_backend_close(const net::TcpConnectionPtr& backend_conn);

    std::string backend_host_;
    unsigned short backend_port_;
    service::Service* service_{nullptr};

    mutable std::mutex mutex_;
    std::unordered_map<const net::TcpConnection*, net::TcpConnectionPtr> client_to_backend_;
    std::unordered_map<const net::TcpConnection*, net::TcpConnectionPtr> backend_to_client_;
};

}  // namespace gateway
}  // namespace chwell
