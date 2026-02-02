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

// JSON编解码器（占位实现，实际需要JSON库）
class JsonCodec : public Codec {
public:
    virtual std::vector<char> encode(const std::string& message) override {
        // TODO: 实际实现需要JSON库（如rapidjson）
        // 这里先返回原始字符串
        return std::vector<char>(message.begin(), message.end());
    }

    virtual std::vector<std::string> decode(const std::vector<char>& data) override {
        // TODO: 实际实现需要JSON库
        std::string msg(data.begin(), data.end());
        return {msg};
    }
};

// Protobuf编解码器（占位实现，实际需要protobuf库）
class ProtobufCodec : public Codec {
public:
    virtual std::vector<char> encode(const std::string& message) override {
        // TODO: 实际实现需要protobuf库
        return std::vector<char>(message.begin(), message.end());
    }

    virtual std::vector<std::string> decode(const std::vector<char>& data) override {
        // TODO: 实际实现需要protobuf库
        std::string msg(data.begin(), data.end());
        return {msg};
    }
};

} // namespace codec
} // namespace chwell
