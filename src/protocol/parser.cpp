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

        // 解析一个完整的消息
        Message msg;
        if (deserialize(std::vector<char>(buffer_.begin(), buffer_.begin() + 4 + body_len), msg)) {
            messages.push_back(msg);
        }

        // 移除已处理的数据
        buffer_.erase(buffer_.begin(), buffer_.begin() + 4 + body_len);
    }

    return messages;
}

} // namespace protocol
} // namespace chwell
