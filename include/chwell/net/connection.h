#pragma once

#include <string>

namespace chwell {
namespace net {

// 连接接口：抽象所有连接类型的共同功能
// 包括 TcpConnection 和 WsConnection
class IConnection {
public:
    virtual ~IConnection() = default;

    // 获取原生句柄
    virtual int native_handle() const = 0;

    // 获取连接描述
    virtual std::string description() const = 0;
};

} // namespace net
} // namespace chwell
