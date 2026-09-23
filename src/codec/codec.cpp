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
    // 仅在首次或容量不足时 reserve，避免每次调用都 reserve
    if (buf.capacity() < 4096) {
        buf.reserve(4096);
    }
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

    // 返回副本而非 std::move(buf)，保留 thread_local buffer 的 capacity
    return buf;
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
        // 防溢出：4 + body_len 在 body_len 接近 UINT32_MAX 时回绕
        if (body_len > max_body_len_ ||
            ring_.readable() < 4u ||
            ring_.readable() - 4u < body_len) {
            if (body_len > max_body_len_) {
                ring_.clear();
            }
            break;
        }

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

    // 返回副本而非 std::move(buf)，保留 thread_local buffer 的 capacity
    return buf;
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
        // 防溢出：4 + body_len 在 body_len 接近 UINT32_MAX 时回绕
        if (body_len > max_body_len_ ||
            ring_.readable() < 4u ||
            ring_.readable() - 4u < body_len) {
            if (body_len > max_body_len_) {
                ring_.clear();
            }
            break;
        }

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

    // 返回副本而非 std::move(buf)，保留 thread_local buffer 的 capacity
    return buf;
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

    ring_.write(data.data(), data.size());

    while (true) {
        std::uint32_t len = 0;

        if (has_pending_) {
            // 上一轮已消费 varint，这里只等 body
            len = pending_len_;
            if (ring_.readable() < len) {
                break;
            }
            has_pending_ = false;
            pending_len_ = 0;
        } else {
            if (ring_.readable() == 0) {
                break;
            }
            if (!parse_varint32(len)) {
                break;
            }
            if (len > max_body_len_) {
                CHWELL_LOG_ERROR("ProtobufCodec: body too large: " << len
                                 << " > max=" << max_body_len_);
                ring_.clear();
                has_pending_ = false;
                pending_len_ = 0;
                break;
            }
            if (ring_.readable() < len) {
                // varint 已消费，暂存长度；不可把后续 body 当新 varint 解析
                pending_len_ = len;
                has_pending_ = true;
                break;
            }
        }

        std::string msg(len, '\0');
        ring_.read(&msg[0], len);
        result.push_back(std::move(msg));
    }

    return result;
}

} // namespace codec
} // namespace chwell
