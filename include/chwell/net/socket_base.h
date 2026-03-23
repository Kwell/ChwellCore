#pragma once

#include <memory>

namespace chwell {
namespace net {

class UdpSocket;
using UdpSocketPtr = std::shared_ptr<UdpSocket>;

} // namespace net
} // namespace chwell
