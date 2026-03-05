#include "chwell/codec/codec.h"
#include "chwell/core/endian.h"
#include <cstring>
#include <cstdint>

namespace chwell {
namespace codec {

std::vector<char> LengthHeaderCodec::encode(const std::string& message) {
    std::uint32_t len = static_cast<std::uint32_t>(message.size());
    std::uint32_t len_net = core::host_to_net32(len);

    std::vector<char> result(4 + message.size());
    std::memcpy(&result[0], &len_net, 4);
    if (!message.empty()) {
        std::memcpy(&result[4], message.data(), message.size());
    }
    return result;
}

std::vector<std::string> LengthHeaderCodec::decode(const std::vector<char>& data) {
    std::vector<std::string> messages;

    buffer_.insert(buffer_.end(), data.begin(), data.end());

    while (true) {
        if (buffer_.size() < 4) {
            break;
        }

        std::uint32_t len_net;
        std::memcpy(&len_net, &buffer_[0], 4);
        std::uint32_t body_len = core::net_to_host32(len_net);

        if (buffer_.size() < 4 + body_len) {
            break;
        }

        std::string msg(buffer_.begin() + 4, buffer_.begin() + 4 + body_len);
        messages.push_back(msg);

        buffer_.erase(buffer_.begin(), buffer_.begin() + 4 + body_len);
    }

    return messages;
}

namespace {

inline void append_varint32(std::vector<char>& out, std::uint32_t value) {
    while (value >= 0x80u) {
        out.push_back(static_cast<char>((value & 0x7Fu) | 0x80u));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value & 0x7Fu));
}

// 从 buffer[pos...] 解析一个 varint32，成功则写 len 和新位置；失败返回 false
inline bool parse_varint32(const std::vector<char>& buf,
                           std::size_t& pos,
                           std::uint32_t& len) {
    std::uint32_t result = 0;
    int shift = 0;
    while (pos < buf.size() && shift <= 28) {
        unsigned char byte = static_cast<unsigned char>(buf[pos++]);
        result |= static_cast<std::uint32_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            len = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

} // anonymous namespace

std::vector<char> ProtobufCodec::encode(const std::string& message) {
    std::vector<char> out;
    out.reserve(5 + message.size());
    append_varint32(out, static_cast<std::uint32_t>(message.size()));
    if (!message.empty()) {
        out.insert(out.end(), message.begin(), message.end());
    }
    return out;
}

std::vector<std::string> ProtobufCodec::decode(const std::vector<char>& data) {
    std::vector<std::string> result;
    buffer_.insert(buffer_.end(), data.begin(), data.end());

    std::size_t pos = 0;
    while (pos < buffer_.size()) {
        std::size_t saved_pos = pos;
        std::uint32_t len = 0;
        if (!parse_varint32(buffer_, pos, len)) {
            // 不完整的长度字段，等待更多数据
            pos = saved_pos;
            break;
        }
        if (buffer_.size() - pos < len) {
            // body 不完整，回退到长度起始位置等待更多数据
            pos = saved_pos;
            break;
        }
        std::string msg(buffer_.begin() + static_cast<std::ptrdiff_t>(pos),
                        buffer_.begin() + static_cast<std::ptrdiff_t>(pos + len));
        result.push_back(msg);
        pos += len;
    }

    if (pos > 0) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(pos));
    }

    return result;
}

} // namespace codec
} // namespace chwell
