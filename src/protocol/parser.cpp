#include "chwell/protocol/parser.h"
#include "chwell/core/endian.h"
#include <cstring>

namespace chwell {
namespace protocol {

std::vector<Message> Parser::feed(const std::vector<char>& data) {
    std::vector<Message> messages;

    // 将新数据追加到缓冲区
    buffer_.insert(buffer_.end(), data.begin(), data.end());

    // 循环解析，直到无法解析出完整消息
    while (true) {
        if (buffer_.size() < 4) {
            break; // 至少需要 4 字节
        }

        // 读取 len
        std::uint16_t len_net;
        std::memcpy(&len_net, &buffer_[2], 2);
        std::uint16_t body_len = core::net_to_host16(len_net);

        // 检查是否有完整的消息（4 字节头部 + body）
        if (buffer_.size() < 4 + body_len) {
            break; // 数据不完整，等待更多数据
        }

        // 直接在 buffer_ 上原地读取，无需构造临时 vector
        Message msg;
        std::uint16_t cmd_net;
        std::memcpy(&cmd_net, &buffer_[0], 2);
        msg.cmd = core::net_to_host16(cmd_net);
        msg.body.assign(buffer_.begin() + 4, buffer_.begin() + 4 + body_len);
        messages.push_back(std::move(msg));

        // 移除已处理的数据
        buffer_.erase(buffer_.begin(), buffer_.begin() + 4 + body_len);
    }

    return messages;
}

} // namespace protocol
} // namespace chwell
