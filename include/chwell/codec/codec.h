#pragma once

#include <vector>
#include <string>
#include <memory>

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
};

// 长度头编解码器：| length (4 bytes, network byte order) | body (length bytes) |
class LengthHeaderCodec : public Codec {
public:
    LengthHeaderCodec() : buffer_() {}

    virtual std::vector<char> encode(const std::string& message) override;
    virtual std::vector<std::string> decode(const std::vector<char>& data) override;
    virtual void reset() override { buffer_.clear(); }

private:
    std::vector<char> buffer_;
};

// JSON 编解码器：使用 4 字节长度前缀（网络字节序）成帧，与 LengthHeaderCodec 一致。
// message 为 UTF-8 JSON 字符串，便于游戏逻辑中直接使用 JSON 文本。
class JsonCodec : public Codec {
public:
    JsonCodec() : buffer_() {}

    virtual std::vector<char> encode(const std::string& message) override;
    virtual std::vector<std::string> decode(const std::vector<char>& data) override;
    virtual void reset() override { buffer_.clear(); }

private:
    std::vector<char> buffer_;
};

// Protobuf 编解码器：varint32 长度前缀流式格式
// [len(varint32)][protobuf bytes][len(varint32)][protobuf bytes]...
// message 为单条 protobuf 消息的二进制序列化结果（如 msg.SerializeAsString()）。
class ProtobufCodec : public Codec {
public:
    ProtobufCodec() : buffer_() {}

    virtual std::vector<char> encode(const std::string& message) override;
    virtual std::vector<std::string> decode(const std::vector<char>& data) override;
    virtual void reset() override { buffer_.clear(); }

private:
    std::vector<char> buffer_;
};

} // namespace codec
} // namespace chwell
