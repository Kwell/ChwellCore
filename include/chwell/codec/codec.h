#pragma once

#include <vector>
#include <string>
#include <memory>
#include <cstdint>

#include "chwell/core/ring_buffer.h"

namespace chwell {
namespace codec {

// 编解码器接口：将高层消息对象与字节流互相转换
class Codec {
public:
    virtual ~Codec() {}

    // 编码：将消息对象序列化为字节流
    virtual std::vector<char> encode(const std::string& message) = 0;

    // 解码：从字节流中解析出消息对象（可能返回多个消息）
    virtual std::vector<std::string> decode(const std::vector<char>& data) = 0;

    // 重置解码器状态（例如连接断开时）
    virtual void reset() {}

    // 设置最大包体长度（防半包攻击）
    virtual void set_max_body_len(std::uint32_t max_bytes) { max_body_len_ = max_bytes; }
    std::uint32_t max_body_len() const { return max_body_len_; }

protected:
    std::uint32_t max_body_len_ = 65536;  // 默认 64KB
};

// 长度头编解码器：| length (4 bytes, network byte order) | body (length bytes) |
// 使用 RingBuffer 零拷贝缓冲区，消除 O(N) 内存搬移
class LengthHeaderCodec : public Codec {
public:
    LengthHeaderCodec() : ring_(4096) {}

    virtual std::vector<char> encode(const std::string& message) override;
    virtual std::vector<std::string> decode(const std::vector<char>& data) override;
    virtual void reset() override { ring_.clear(); }

private:
    core::RingBuffer ring_;
};

// JSON 编解码器：使用 4 字节长度前缀（网络字节序）成帧，与 LengthHeaderCodec 一致。
// 使用 RingBuffer 零拷贝缓冲区
class JsonCodec : public Codec {
public:
    JsonCodec() : ring_(4096) {}

    virtual std::vector<char> encode(const std::string& message) override;
    virtual std::vector<std::string> decode(const std::vector<char>& data) override;
    virtual void reset() override { ring_.clear(); }

private:
    core::RingBuffer ring_;
};

// Protobuf 编解码器：varint32 长度前缀流式格式
// [len(varint32)][protobuf bytes][len(varint32)][protobuf bytes]...
// 使用 RingBuffer 零拷贝缓冲区
class ProtobufCodec : public Codec {
public:
    ProtobufCodec() : ring_(4096) {}

    virtual std::vector<char> encode(const std::string& message) override;
    virtual std::vector<std::string> decode(const std::vector<char>& data) override;
    virtual void reset() override { ring_.clear(); }

private:
    // 从 RingBuffer 解析 varint32
    bool parse_varint32(std::uint32_t& len);

    core::RingBuffer ring_;
};

} // namespace codec
} // namespace chwell
