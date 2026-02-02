#include "chwell/codec/codec.h"
#include "chwell/core/endian.h"
#include <cstring>

namespace chwell {
namespace codec {

std::vector<char> LengthHeaderCodec::encode(const std::string& message) {
    std::vector<char> result;
    std::uint32_t len = static_cast<std::uint32_t>(message.size());
    std::uint32_t len_net = core::host_to_net32(len);

    result.reserve(4 + message.size());
    result.insert(result.end(), reinterpret_cast<const char*>(&len_net),
                  reinterpret_cast<const char*>(&len_net) + 4);
    result.insert(result.end(), message.begin(), message.end());

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

} // namespace codec
} // namespace chwell
