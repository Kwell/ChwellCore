#pragma once

#include <cstdint>

// 统一的字节序转换封装，避免上层直接依赖平台 socket 头文件。

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace chwell {
namespace core {

inline std::uint16_t host_to_net16(std::uint16_t v) {
    return htons(v);
}

inline std::uint16_t net_to_host16(std::uint16_t v) {
    return ntohs(v);
}

inline std::uint32_t host_to_net32(std::uint32_t v) {
    return htonl(v);
}

inline std::uint32_t net_to_host32(std::uint32_t v) {
    return ntohl(v);
}

} // namespace core
} // namespace chwell

