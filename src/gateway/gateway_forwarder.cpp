#include "chwell/gateway/gateway_forwarder.h"
#include "chwell/core/logger.h"
#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/protocol/message.h"
#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_connection.h"

#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <cstdint>

namespace chwell {
namespace gateway {

GatewayForwarderComponent::GatewayForwarderComponent(
    const std::string& backend_host, unsigned short backend_port)
    : backend_host_(backend_host)
    , backend_port_(backend_port)
    , cluster_config_path_("") {}

GatewayForwarderComponent::GatewayForwarderComponent(
    const std::string& node_type, const std::string& cluster_config_path)
    : backend_node_type_(node_type)
    , cluster_config_path_(cluster_config_path.empty() ? "config/cluster.yaml"
                                                        : cluster_config_path) {}

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

    if (!backend_node_type_.empty()) {
        // Ensure registry is loaded (lazy init, not static globals)
        if (!registry_loaded_) {
            if (!registry_.load_from_yaml_file(cluster_config_path_)) {
                CHWELL_LOG_WARN("Gateway: failed to load cluster config from '"
                                + cluster_config_path_ + "', fallback to direct host/port");
            }
            registry_loaded_ = true;
        }

        // Use connection pointer address as hash key for stable sharding per client
        std::string hash_key = std::to_string(
            reinterpret_cast<uintptr_t>(client_conn.get()));

        cluster::NodeInfo info;
        if (registry_.select_node_by_hash(hash_key, info, backend_node_type_)) {
            host = info.listen_addr;
            port = info.listen_port;
        } else {
            CHWELL_LOG_WARN("Gateway: no backend node found for type=" + backend_node_type_
                            + ", fallback to direct backend host/port");
        }
    }

    return do_connect(client_conn, host, port);
}

net::TcpConnectionPtr GatewayForwarderComponent::do_connect(
    const net::TcpConnectionPtr& client_conn,
    const std::string& host, unsigned short port) {

    if (host.empty() || port == 0) {
        CHWELL_LOG_ERROR("Gateway: invalid backend address " + host + ":"
                         + std::to_string(port));
        return nullptr;
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
        CHWELL_LOG_ERROR("Gateway: connect to backend " + host + ":"
                         + std::to_string(port) + " failed: " + strerror(errno));
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

    CHWELL_LOG_INFO("Gateway: connected to backend " + host + ":"
                    + std::to_string(port));
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
    net::TcpConnectionPtr client_conn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = backend_to_client_.find(backend_conn.get());
        if (it != backend_to_client_.end()) {
            client_conn = it->second;
            client_to_backend_.erase(it->second.get());
            backend_to_client_.erase(it);
        }
    }
    CHWELL_LOG_INFO("Gateway: backend connection closed");

    // If using node_type mode, attempt reconnect to another available node
    if (client_conn && !backend_node_type_.empty()) {
        CHWELL_LOG_INFO("Gateway: attempting reconnect for client after backend close");
        // The next forward() call will trigger connect_backend() again,
        // which will pick a new node via hash. No explicit action needed here.
    }
}

}  // namespace gateway
}  // namespace chwell
