#include "chwell/gateway/gateway_forwarder.h"
#include "chwell/core/logger.h"
#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/protocol/message.h"
#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_connection.h"
#include "chwell/cluster/node_registry.h"

#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace chwell {
namespace gateway {

GatewayForwarderComponent::GatewayForwarderComponent(
    const std::string& backend_host, unsigned short backend_port)
    : backend_host_(backend_host), backend_port_(backend_port) {}

GatewayForwarderComponent::GatewayForwarderComponent(const std::string& node_type)
    : backend_node_type_(node_type) {}

void GatewayForwarderComponent::on_register(service::Service& svc) {
    service_ = &svc;
}

void GatewayForwarderComponent::on_disconnect(const net::TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = client_to_backend_.find(conn.get());
    if (it != client_to_backend_.end()) {
        net::TcpConnectionPtr backend = it->second;
        backend_to_client_.erase(backend.get());
        client_to_backend_.erase(it);
        backend->close();
        CHWELL_LOG_INFO("Gateway: closed backend connection for client disconnect");
    }
}

bool GatewayForwarderComponent::has_backend(const net::TcpConnectionPtr& client_conn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return client_to_backend_.find(client_conn.get()) != client_to_backend_.end();
}

net::TcpConnectionPtr GatewayForwarderComponent::connect_backend(
    const net::TcpConnectionPtr& client_conn) {
    std::string host = backend_host_;
    unsigned short port = backend_port_;

    // 如果指定了 node_type，则尝试通过全局 NodeRegistry 进行节点发现
    if (!backend_node_type_.empty()) {
        static cluster::NodeRegistry s_registry;
        static bool s_loaded = false;
        if (!s_loaded) {
            // 这里采用简单的静态 YAML 配置，后续可替换为 etcd/Consul 等动态发现
            if (!s_registry.load_from_yaml_file("config/cluster.yaml")) {
                CHWELL_LOG_WARN("Gateway: failed to load cluster config, fallback to direct backend host/port");
            }
            s_loaded = true;
        }

        std::string key = "client";
        cluster::NodeInfo info;
        if (s_registry.select_node_by_hash(key, info, backend_node_type_)) {
            host = info.listen_addr;
            port = info.listen_port;
        } else {
            CHWELL_LOG_WARN("Gateway: no backend node found for type=" + backend_node_type_ +
                            ", fallback to direct backend host/port");
        }
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        CHWELL_LOG_ERROR("Gateway: socket failed");
        return nullptr;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        CHWELL_LOG_ERROR("Gateway: invalid backend address " + host);
        close(fd);
        return nullptr;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        CHWELL_LOG_ERROR("Gateway: connect to backend failed: " +
                                       std::string(strerror(errno)));
        close(fd);
        return nullptr;
    }

    net::TcpSocket socket(fd);
    net::TcpConnectionPtr backend = std::make_shared<net::TcpConnection>(std::move(socket));

    backend->set_message_callback([this](const net::TcpConnectionPtr& conn,
                                         const std::vector<char>& data) {
        on_backend_message(conn, data);
    });
    backend->set_close_callback([this](const net::TcpConnectionPtr& conn) {
        on_backend_close(conn);
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        client_to_backend_[client_conn.get()] = backend;
        backend_to_client_[backend.get()] = client_conn;
    }

    service_->io_service().post([backend]() { backend->start(); });

    CHWELL_LOG_INFO("Gateway: connected to backend " + host + ":" +
                                  std::to_string(port));
    return backend;
}

void GatewayForwarderComponent::forward(const net::TcpConnectionPtr& client_conn,
                                       const protocol::Message& msg) {
    net::TcpConnectionPtr backend;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = client_to_backend_.find(client_conn.get());
        if (it != client_to_backend_.end()) {
            backend = it->second;
        }
    }

    if (!backend) {
        backend = connect_backend(client_conn);
        if (!backend) {
            CHWELL_LOG_ERROR("Gateway: failed to connect backend");
            protocol::Message err_reply(msg.cmd, "gateway: backend unavailable");
            service::ProtocolRouterComponent::send_message(client_conn, err_reply);
            return;
        }
    }

    std::vector<char> data = protocol::serialize(msg);
    backend->send(data);
}

void GatewayForwarderComponent::on_backend_message(
    const net::TcpConnectionPtr& backend_conn, const std::vector<char>& data) {
    net::TcpConnectionPtr client_conn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = backend_to_client_.find(backend_conn.get());
        if (it != backend_to_client_.end()) {
            client_conn = it->second;
        }
    }

    if (client_conn) {
        client_conn->send(data);
    }
}

void GatewayForwarderComponent::on_backend_close(const net::TcpConnectionPtr& backend_conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backend_to_client_.find(backend_conn.get());
    if (it != backend_to_client_.end()) {
        client_to_backend_.erase(it->second.get());
        backend_to_client_.erase(it);
        CHWELL_LOG_INFO("Gateway: backend connection closed");
    }
}

}  // namespace gateway
}  // namespace chwell
