#include "chwell/protocol/message.h"
#include "chwell/core/endian.h"
#include <cstring>

namespace chwell {
namespace protocol {

std::vector<char> serialize(const Message& msg) {
    std::vector<char> result;
    result.reserve(4 + msg.body.size());

    // cmd (2 bytes, network byte order)
    std::uint16_t cmd_net = core::host_to_net16(msg.cmd);
    result.insert(result.end(), reinterpret_cast<const char*>(&cmd_net),
                  reinterpret_cast<const char*>(&cmd_net) + 2);

    // len (2 bytes, network byte order)
    std::uint16_t len_net = core::host_to_net16(static_cast<std::uint16_t>(msg.body.size()));
    result.insert(result.end(), reinterpret_cast<const char*>(&len_net),
                  reinterpret_cast<const char*>(&len_net) + 2);

    // body
    result.insert(result.end(), msg.body.begin(), msg.body.end());

    return result;
}

bool deserialize(const std::vector<char>& data, Message& msg) {
    if (data.size() < 4) {
        return false; // 至少需要 4 字节（cmd + len）
    }

    // 读取 cmd
    std::uint16_t cmd_net;
    std::memcpy(&cmd_net, &data[0], 2);
    msg.cmd = core::net_to_host16(cmd_net);

    // 读取 len
    std::uint16_t len_net;
    std::memcpy(&len_net, &data[2], 2);
    std::uint16_t body_len = core::net_to_host16(len_net);

    // 检查是否有完整的 body
    if (data.size() < 4 + body_len) {
        return false; // 数据不完整
    }

    // 读取 body
    msg.body.assign(data.begin() + 4, data.begin() + 4 + body_len);
    return true;
}

} // namespace protocol
} // namespace chwell
