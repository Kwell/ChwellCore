#include "chwell/protocol/parser.h"
#include "chwell/core/endian.h"
#include <cstring>

namespace chwell {
namespace protocol {

void Parser::compact_prefix() {
    if (head_ == 0) {
        return;
    }
    if (head_ >= 4096 && head_ * 2 >= buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0;
    }
}

std::vector<Message> Parser::feed(std::string_view data) {
    std::vector<Message> messages;

    // 将新数据追加到缓冲区
    buffer_.insert(buffer_.end(), data.begin(), data.end());

    // 循环解析，直到无法解析出完整消息
    while (true) {
        std::size_t avail = buffer_.size() - head_;
        if (avail < 4) {
            break; // 至少需要 4 字节
        }

        // 读取 len
        std::uint16_t len_net;
        std::memcpy(&len_net, buffer_.data() + head_ + 2, 2);
        std::uint16_t body_len = core::net_to_host16(len_net);

        // 检查是否有完整的消息（4 字节头部 + body）
        if (avail < 4 + body_len) {
            break; // 数据不完整，等待更多数据
        }

        Message msg;
        std::uint16_t cmd_net;
        std::memcpy(&cmd_net, buffer_.data() + head_, 2);
        msg.cmd = core::net_to_host16(cmd_net);
        msg.body.assign(buffer_.data() + head_ + 4, buffer_.data() + head_ + 4 + body_len);
        messages.push_back(std::move(msg));

        head_ += 4 + body_len;
    }

    compact_prefix();
    return messages;
}

} // namespace protocol
} // namespace chwell
