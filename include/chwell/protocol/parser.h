#pragma once

#include <vector>
#include <string_view>
#include <cstdint>
#include "chwell/protocol/message.h"

namespace chwell {
namespace protocol {

// 协议解析器：处理粘包/拆包问题
// 内部维护一个缓冲区，累积接收到的数据，直到能解析出完整的消息
class Parser {
public:
    Parser() : buffer_(), head_(0) {}

    // 添加新接收到的数据，尝试解析出完整的消息
    // 返回解析出的消息列表（可能为空，也可能有多个）
    std::vector<Message> feed(std::string_view data);
    std::vector<Message> feed(const std::vector<char>& data) {
        return feed(std::string_view(data.data(), data.size()));
    }

    // 清空缓冲区（例如连接断开时）
    void reset() {
        buffer_.clear();
        head_ = 0;
    }

private:
    void compact_prefix();

    std::vector<char> buffer_;
    std::size_t head_;
};

} // namespace protocol
} // namespace chwell
