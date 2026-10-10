#include <gtest/gtest.h>
#include "chwell/cluster/tcp_rpc_transport.h"
#include "chwell/protocol/message.h"
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <thread>

namespace {
bool receive_exact(int fd, char* data, std::size_t size) {
    while (size) {
        pollfd pending{fd, POLLIN, 0};
        if (::poll(&pending, 1, 1000) <= 0) return false;
        const auto count = ::recv(fd, data, size, 0);
        if (count <= 0) return false;
        data += count; size -= static_cast<std::size_t>(count);
    }
    return true;
}
class RpcPeer {
public:
    RpcPeer() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (fd_ < 0 || ::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address))) return;
        socklen_t size = sizeof(address);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &size)) return;
        node.node_id = "peer"; node.listen_addr = "127.0.0.1"; node.listen_port = ntohs(address.sin_port);
    }
    ~RpcPeer() { if (thread_.joinable()) thread_.join(); if (fd_ >= 0) ::close(fd_); }
    bool serve(const std::string& mode, int requests = 1) {
        if (::listen(fd_, 1)) return false;
        thread_ = std::thread([this, mode, requests] {
            pollfd listener{fd_, POLLIN, 0};
            if (::poll(&listener, 1, 2000) <= 0) return;
            const int accepted = ::accept(fd_, nullptr, nullptr);
            if (accepted < 0) return;
            struct Guard { int fd; ~Guard() { ::close(fd); } } guard{accepted};
            for (int i = 0; i < requests; ++i) {
                char header[4];
                if (!receive_exact(accepted, header, sizeof(header))) return;
                std::uint16_t cmd, size;
                std::memcpy(&cmd, header, 2); std::memcpy(&size, header + 2, 2);
                std::vector<char> body(ntohs(size));
                if (body.size() < 4 || !receive_exact(accepted, body.data(), body.size())) return;
                if (mode == "timeout") {
                    pollfd pending{accepted, POLLIN, 0};
                    ::poll(&pending, 1, 300);
                    return;
                }
                if (mode == "wrong-id") body[0] ^= 1;
                auto response = chwell::protocol::serialize(chwell::protocol::Message(ntohs(cmd), body));
                if (mode == "truncated") response.resize(5);
                // Split the header across writes, then send the body.
                if (::send(accepted, response.data(), 2, MSG_NOSIGNAL) != 2) return;
                if (::send(accepted, response.data() + 2, response.size() - 2, MSG_NOSIGNAL) <= 0) return;
                if (mode == "truncated") return;
            }
        });
        return true;
    }
    chwell::cluster::NodeInfo node;
private:
    int fd_ = -1;
    std::thread thread_;
};
}

TEST(TcpRpcTransportTest, CorrelatedResponsesStripPrefixAndReuseConnection) {
    RpcPeer peer;
    ASSERT_GT(peer.node.listen_port, 0);
    ASSERT_TRUE(peer.serve("echo", 2));
    chwell::cluster::TcpRpcTransport transport(peer.node);
    std::vector<char> response;
    for (const auto& payload : {std::vector<char>{'h', 'i'}, std::vector<char>{}}) {
        ASSERT_TRUE(transport.call(42, payload, response, 1000));
        EXPECT_EQ(response, payload);
    }
}

TEST(TcpRpcTransportTest, WrongCorrelationAndPartialRepliesCloseTheConnection) {
    for (const auto& mode : {"wrong-id", "truncated"}) {
        RpcPeer peer;
        ASSERT_GT(peer.node.listen_port, 0);
        ASSERT_TRUE(peer.serve(mode));
        chwell::cluster::TcpRpcTransport transport(peer.node);
        std::vector<char> response{'o', 'l', 'd'};
        EXPECT_FALSE(transport.call(1, {'x'}, response, 1000));
        EXPECT_FALSE(transport.healthy());
        EXPECT_EQ(response, (std::vector<char>{'o', 'l', 'd'}));
    }
}

TEST(TcpRpcTransportTest, OneDeadlineBoundsAStalledPeer) {
    RpcPeer peer;
    ASSERT_GT(peer.node.listen_port, 0);
    ASSERT_TRUE(peer.serve("timeout"));
    chwell::cluster::TcpRpcTransport transport(peer.node);
    std::vector<char> response;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(transport.call(1, {'x'}, response, 50));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
    EXPECT_FALSE(transport.healthy());
}

TEST(TcpRpcTransportTest, RefusedConnectionAndInvalidRequestsFailWithoutAlteringResponse) {
    RpcPeer peer; // No listen: connection refused.
    ASSERT_GT(peer.node.listen_port, 0);
    chwell::cluster::TcpRpcTransport transport(peer.node);
    std::vector<char> response{'o', 'l', 'd'};
    EXPECT_FALSE(transport.call(1, {}, response, 0));
    EXPECT_FALSE(transport.call(1, std::vector<char>(65532), response, 100));
    EXPECT_FALSE(transport.call(1, {}, response, 100));
    EXPECT_EQ(response, (std::vector<char>{'o', 'l', 'd'}));
    EXPECT_FALSE(transport.healthy());
}
