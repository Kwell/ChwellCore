#pragma once

#include <string>
#include <asio.hpp>

namespace chwell {
namespace cluster {

class Node {
public:
    Node(asio::io_service& io_service,
         const std::string& node_id,
         const std::string& listen_addr,
         unsigned short listen_port)
        : io_service_(io_service),
          node_id_(node_id),
          listen_addr_(listen_addr),
          listen_port_(listen_port) {}

    const std::string& id() const { return node_id_; }

    // TODO: 这里可以扩展为注册中心/发现服务、RPC 等

private:
    asio::io_service& io_service_;
    std::string node_id_;
    std::string listen_addr_;
    unsigned short listen_port_;
};

} // namespace cluster
} // namespace chwell

