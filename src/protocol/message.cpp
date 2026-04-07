#include "chwell/protocol/message.h"
#include "chwell/core/endian.h"
#include <cstring>

namespace chwell {
namespace protocol {

namespace {

// 线程本地序列化缓冲区，避免频繁分配
static constexpr size_t kDefaultBufferSize = 4096;

inline std::vector<char>& get_serialize_buffer() {
    thread_local std::vector<char> buffer;
    buffer.clear();
    if (buffer.capacity() < kDefaultBufferSize) {
        buffer.reserve(kDefaultBufferSize);
    }
    return buffer;
}

inline std::vector<char>& get_deserialize_buffer() {
    thread_local std::vector<char> buffer;
    buffer.clear();
    if (buffer.capacity() < kDefaultBufferSize) {
        buffer.reserve(kDefaultBufferSize);
    }
    return buffer;
}

} // anonymous namespace

std::vector<char> serialize(const Message& msg) {
    auto& buf = get_serialize_buffer();

    size_t needed = 4 + msg.body.size();
    if (buf.capacity() < needed) {
        buf.reserve(needed);
    }

    buf.resize(needed);

    // cmd (2 bytes, network byte order)
    std::uint16_t cmd_net = core::host_to_net16(msg.cmd);
    std::memcpy(buf.data(), &cmd_net, 2);

    // len (2 bytes, network byte order)
    std::uint16_t len_net = core::host_to_net16(static_cast<std::uint16_t>(msg.body.size()));
    std::memcpy(buf.data() + 2, &len_net, 2);

    // body
    if (!msg.body.empty()) {
        std::memcpy(buf.data() + 4, msg.body.data(), msg.body.size());
    }

    return std::move(buf);  // 移动语义，调用方拿到 buffer 所有权
}

bool deserialize(const std::vector<char>& data, Message& msg) {
    if (data.size() < 4) {
        return false;
    }

    // 读取 cmd
    std::uint16_t cmd_net;
    std::memcpy(&cmd_net, &data[0], 2);
    msg.cmd = core::net_to_host16(cmd_net);

    // 读取 len
    std::uint16_t len_net;
    std::memcpy(&len_net, &data[2], 2);
    std::uint16_t body_len = core::net_to_host16(len_net);

    if (data.size() < 4 + body_len) {
        return false;
    }

    // 读取 body（直接赋值，不走 thread_local）
    msg.body.assign(data.data() + 4, data.data() + 4 + body_len);

    return true;
}

} // namespace protocol
} // namespace chwell
