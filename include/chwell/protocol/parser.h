#pragma once

#include <vector>
#include <cstdint>
#include "chwell/protocol/message.h"

namespace chwell {
namespace protocol {

// 协议解析器：处理粘包/拆包问题
// 内部维护一个缓冲区，累积接收到的数据，直到能解析出完整的消息
class Parser {
public:
    Parser() : buffer_() {}

    // 添加新接收到的数据，尝试解析出完整的消息
    // 返回解析出的消息列表（可能为空，也可能有多个）
    std::vector<Message> feed(const std::vector<char>& data);

    // 清空缓冲区（例如连接断开时）
    void reset() { buffer_.clear(); }

private:
    std::vector<char> buffer_;
};

} // namespace protocol
} // namespace chwell
