#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace chwell {
namespace protocol {

// 协议格式：| cmd (2 bytes) | len (2 bytes) | body (len bytes) |
// cmd: 命令号（uint16_t，网络字节序）
// len: body 长度（uint16_t，网络字节序）
// body: 实际数据

struct Message {
    std::uint16_t cmd;
    std::vector<char> body;

    Message() : cmd(0) {}
    Message(std::uint16_t c, const std::vector<char>& b) : cmd(c), body(b) {}
    Message(std::uint16_t c, const std::string& s) : cmd(c), body(s.begin(), s.end()) {}
};

// 将 Message 序列化为字节流（用于发送）
std::vector<char> serialize(const Message& msg);

// 从字节流反序列化 Message（用于接收）
// 返回是否成功解析出一个完整的消息
bool deserialize(const std::vector<char>& data, Message& msg);

} // namespace protocol
} // namespace chwell
