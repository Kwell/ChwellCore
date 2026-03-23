#pragma once

#include <unordered_map>
#include <string>
#include <string_view>
#include <memory>
#include <mutex>

#include "chwell/service/component.h"
#include "chwell/protocol/message.h"
#include "chwell/cluster/node_registry.h"

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
    // 静态单节点后端
    GatewayForwarderComponent(const std::string& backend_host,
                              unsigned short backend_port);

    // 使用 NodeRegistry + node_type 的构造函数
    // cluster_config_path: YAML 配置路径，默认 "config/cluster.yaml"
    explicit GatewayForwarderComponent(const std::string& node_type,
                                       const std::string& cluster_config_path);

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
    net::TcpConnectionPtr do_connect(const net::TcpConnectionPtr& client_conn,
                                     const std::string& host, unsigned short port);
    void on_backend_message(const net::TcpConnectionPtr& backend_conn,
                            std::string_view data);
    void on_backend_close(const net::TcpConnectionPtr& backend_conn);

    std::string backend_host_;
    unsigned short backend_port_{0};
    std::string backend_node_type_;
    std::string cluster_config_path_;
    service::Service* service_{nullptr};

    // Instance-level NodeRegistry (not static globals)
    bool registry_loaded_{false};
    cluster::NodeRegistry registry_;

    mutable std::mutex mutex_;
    std::unordered_map<const net::TcpConnection*, net::TcpConnectionPtr> client_to_backend_;
    std::unordered_map<const net::TcpConnection*, net::TcpConnectionPtr> backend_to_client_;
};

}  // namespace gateway
}  // namespace chwell
