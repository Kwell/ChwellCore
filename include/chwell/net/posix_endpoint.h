#pragma once

#include <cstring>
#include <string>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace chwell { namespace net {

// UDP 端点
struct UdpEndpoint {
    sockaddr_in addr_{};
    UdpEndpoint() { memset(&addr_, 0, sizeof(addr_)); }
    UdpEndpoint(const std::string& ip, unsigned short port) {
        memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr);
    }
    std::string address() const {
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
        return std::string(buf);
    }
    unsigned short port() const { return ntohs(addr_.sin_port); }
};

} } // namespace chwell::net
