#include "chwell/codec/codec.h"
#include "chwell/core/endian.h"
#include "chwell/core/logger.h"
#include <cstring>
#include <cstdint>

namespace chwell {
namespace codec {

namespace {

// 线程本地编码缓冲区
inline std::vector<char>& encode_buffer() {
    thread_local std::vector<char> buf;
    buf.clear();
    buf.reserve(4096);
    return buf;
}

inline void append_varint32(std::vector<char>& out, std::uint32_t value) {
    while (value >= 0x80u) {
        out.push_back(static_cast<char>((value & 0x7Fu) | 0x80u));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value & 0x7Fu));
}

} // anonymous namespace

// ============================================
// LengthHeaderCodec
// ============================================

std::vector<char> LengthHeaderCodec::encode(const std::string& message) {
    auto& buf = encode_buffer();

    std::uint32_t len = static_cast<std::uint32_t>(message.size());
    std::uint32_t len_net = core::host_to_net32(len);
    size_t needed = 4 + message.size();

    if (buf.capacity() < needed) buf.reserve(needed);
    buf.resize(needed);

    std::memcpy(buf.data(), &len_net, 4);
    if (!message.empty()) {
        std::memcpy(buf.data() + 4, message.data(), message.size());
    }

    return std::move(buf);
}

std::vector<std::string> LengthHeaderCodec::decode(const std::vector<char>& data) {
    std::vector<std::string> messages;

    // 🆕 使用 RingBuffer，零拷贝
    ring_.write(data.data(), data.size());

    while (ring_.readable() >= 4) {
        char header[4];
        ring_.peek(header, 4);

        std::uint32_t len_net;
        std::memcpy(&len_net, header, 4);
        std::uint32_t body_len = core::net_to_host32(len_net);

        // 🆕 包体长度校验
        if (body_len > max_body_len_) {
            ring_.clear();  // 丢弃恶意数据
            break;
        }

        if (ring_.readable() < 4 + body_len) break;

        // 🆕 消费 4 字节头
        ring_.consume(4);

        // 读取 body
        std::string body(body_len, '\0');
        ring_.read(&body[0], body_len);

        messages.push_back(std::move(body));
    }

    return messages;
}

// ============================================
// JsonCodec
// ============================================

std::vector<char> JsonCodec::encode(const std::string& message) {
    auto& buf = encode_buffer();

    std::uint32_t len = static_cast<std::uint32_t>(message.size());
    std::uint32_t len_net = core::host_to_net32(len);
    size_t needed = 4 + message.size();

    if (buf.capacity() < needed) buf.reserve(needed);
    buf.resize(needed);

    std::memcpy(buf.data(), &len_net, 4);
    if (!message.empty()) {
        std::memcpy(buf.data() + 4, message.data(), message.size());
    }

    return std::move(buf);
}

std::vector<std::string> JsonCodec::decode(const std::vector<char>& data) {
    std::vector<std::string> messages;

    // 🆕 使用 RingBuffer，零拷贝
    ring_.write(data.data(), data.size());

    while (ring_.readable() >= 4) {
        char header[4];
        ring_.peek(header, 4);

        std::uint32_t len_net;
        std::memcpy(&len_net, header, 4);
        std::uint32_t body_len = core::net_to_host32(len_net);

        // 🆕 包体长度校验
        if (body_len > max_body_len_) {
            ring_.clear();
            break;
        }

        if (ring_.readable() < 4 + body_len) break;

        ring_.consume(4);

        std::string body(body_len, '\0');
        ring_.read(&body[0], body_len);

        messages.push_back(std::move(body));
    }

    return messages;
}

// ============================================
// ProtobufCodec
// ============================================

std::vector<char> ProtobufCodec::encode(const std::string& message) {
    auto& buf = encode_buffer();

    // 预估 varint32 最大 5 字节 + body
    size_t needed = 5 + message.size();
    if (buf.capacity() < needed) buf.reserve(needed);

    buf.clear();
    append_varint32(buf, static_cast<std::uint32_t>(message.size()));
    if (!message.empty()) {
        buf.insert(buf.end(), message.begin(), message.end());
    }

    return std::move(buf);
}

bool ProtobufCodec::parse_varint32(std::uint32_t& len) {
    // 一次 peek 最多 5 字节
    char buf[5];
    size_t avail = ring_.peek(buf, 5);
    if (avail == 0) return false;

    std::uint32_t result = 0;
    int shift = 0;

    for (size_t i = 0; i < avail; ++i) {
        unsigned char byte = static_cast<unsigned char>(buf[i]);
        result |= static_cast<std::uint32_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            ring_.consume(i + 1);
            len = result;
            return true;
        }
        shift += 7;
    }

    return false;  // 不完整
}

std::vector<std::string> ProtobufCodec::decode(const std::vector<char>& data) {
    std::vector<std::string> result;

    // 🆕 使用 RingBuffer，零拷贝
    ring_.write(data.data(), data.size());

    while (ring_.readable() > 0) {
        std::uint32_t len = 0;
        if (!parse_varint32(len)) {
            // varint32 不完整，需要更多数据。恢复位置
            // 注意：parse_varint32 只在成功时 consume
            break;
        }

        // 🆕 包体长度校验
        if (len > max_body_len_) {
            CHWELL_LOG_ERROR("ProtobufCodec: body too large: " << len
                             << " > max=" << max_body_len_);
            ring_.clear();
            break;
        }

        if (ring_.readable() < len) {
            // body 数据不足，需要更多数据
            // parse_varint32 只在成功时 consume，所以 varint 已被消费
            // 下次调用 decode 时 body 数据就会到了
            break;
        }

        std::string msg(len, '\0');
        ring_.read(&msg[0], len);
        result.push_back(std::move(msg));
    }

    return result;
}

} // namespace codec
} // namespace chwell
