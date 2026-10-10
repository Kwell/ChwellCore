#pragma once

#include "chwell/cluster/rpc_router.h"

namespace chwell { namespace cluster {

// Synchronous POSIX transport for the existing RPC wire protocol. One connection
// per transport; calls are serialized. Uses numeric IPv4 endpoints and bounds
// connect/send/receive by one call deadline. No TLS or automatic request replay.
class TcpRpcTransport : public RpcTransport {
public:
    explicit TcpRpcTransport(NodeInfo node) : node_(std::move(node)) {}
    ~TcpRpcTransport() override;
    bool call(std::uint16_t cmd, const std::vector<char>& request,
              std::vector<char>& response, int timeout_ms) override;
    bool healthy() const override { return healthy_.load(); }
private:
    bool close_failed();
    NodeInfo node_;
    std::mutex mutex_;
    int fd_ = -1;
    std::uint32_t next_id_ = 1;
    std::atomic<bool> healthy_{true};
};

} } // namespace chwell::cluster
