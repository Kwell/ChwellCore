#pragma once

#include <string>

namespace chwell {
namespace net {

// 基础连接接口：仅提供原生句柄与描述的最小抽象
// 注意：功能更完整的接口请使用 connection_adapter.h 中的 IConnection
class IBaseConnection {
public:
    virtual ~IBaseConnection() = default;

    // 获取原生句柄
    virtual int native_handle() const = 0;

    // 获取连接描述
    virtual std::string description() const = 0;
};

} // namespace net
} // namespace chwell
