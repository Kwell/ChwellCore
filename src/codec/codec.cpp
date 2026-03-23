#include "chwell/codec/codec.h"
#include "chwell/core/endian.h"
#include <cstring>
#include <cstdint>

namespace chwell {
namespace codec {

void LengthHeaderCodec::compact_prefix() {
    if (head_ == 0) {
        return;
    }
    // 已消费前缀较大时前移，避免每帧 O(n) erase
    if (head_ >= 4096 && head_ * 2 >= buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0;
    }
}

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
        std::size_t avail = buffer_.size() - head_;
        if (avail < 4) {
            break;
        }

        std::uint32_t len_net;
        std::memcpy(&len_net, buffer_.data() + head_, 4);
        std::uint32_t body_len = core::net_to_host32(len_net);

        if (avail < 4 + body_len) {
            break;
        }

        messages.emplace_back(buffer_.data() + head_ + 4, body_len);
        head_ += 4 + body_len;
    }

    compact_prefix();
    return messages;
}

void JsonCodec::compact_prefix() {
    if (head_ == 0) {
        return;
    }
    if (head_ >= 4096 && head_ * 2 >= buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0;
    }
}

std::vector<char> JsonCodec::encode(const std::string& message) {
    std::uint32_t len = static_cast<std::uint32_t>(message.size());
    std::uint32_t len_net = core::host_to_net32(len);

    std::vector<char> result(4 + message.size());
    std::memcpy(&result[0], &len_net, 4);
    if (!message.empty()) {
        std::memcpy(&result[4], message.data(), message.size());
    }
    return result;
}

std::vector<std::string> JsonCodec::decode(const std::vector<char>& data) {
    std::vector<std::string> messages;

    buffer_.insert(buffer_.end(), data.begin(), data.end());

    while (true) {
        std::size_t avail = buffer_.size() - head_;
        if (avail < 4) {
            break;
        }

        std::uint32_t len_net;
        std::memcpy(&len_net, buffer_.data() + head_, 4);
        std::uint32_t body_len = core::net_to_host32(len_net);

        if (avail < 4 + body_len) {
            break;
        }

        messages.emplace_back(buffer_.data() + head_ + 4, body_len);
        head_ += 4 + body_len;
    }

    compact_prefix();
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

void ProtobufCodec::compact_prefix() {
    if (head_ == 0) {
        return;
    }
    if (head_ >= 4096 && head_ * 2 >= buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0;
    }
}

std::vector<std::string> ProtobufCodec::decode(const std::vector<char>& data) {
    std::vector<std::string> result;
    buffer_.insert(buffer_.end(), data.begin(), data.end());

    std::size_t pos = head_;
    while (pos < buffer_.size()) {
        std::size_t saved_pos = pos;
        std::uint32_t len = 0;
        if (!parse_varint32(buffer_, pos, len)) {
            pos = saved_pos;
            break;
        }
        if (buffer_.size() - pos < len) {
            pos = saved_pos;
            break;
        }
        result.emplace_back(buffer_.data() + pos, len);
        pos += len;
    }

    head_ = pos;
    compact_prefix();
    return result;
}

} // namespace codec
} // namespace chwell
