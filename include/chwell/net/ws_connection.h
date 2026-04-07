// ============================================================================
// WARNING: This is a raw TCP wrapper, NOT a WebSocket implementation.
// It does NOT implement RFC 6455 (WebSocket Protocol). There is no HTTP
// upgrade handshake, no opcode framing, no mask/unmask, no ping/pong, and
// no close status codes. The name "WsRawConnection" exists for historical
// reasons and backward compatibility — it should be treated as a raw TCP
// stream.
//
// Additionally, this class provides NO message boundary handling. It delivers
// raw bytes as they arrive from the kernel TCP buffer. The upper layer MUST
// implement its own framing protocol (e.g. length-prefix, delimiter, or a
// proper WebSocket library) if message semantics are needed.
//
// Thread safety notes:
// - Callbacks (message_cb_, close_cb_) must be set BEFORE calling start().
//   They are not protected by any lock and should not be changed while the
//   read loop is running.
// ============================================================================

#pragma once

#include <memory>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

#include "chwell/net/posix_io.h"

namespace chwell {
namespace net {

class WsRawConnection;

typedef std::shared_ptr<WsRawConnection> WsConnectionPtr;
typedef std::function<void(const WsConnectionPtr&, const std::string&)> WsMessageCallback;
typedef std::function<void(const WsConnectionPtr&)> WsConnectionCallback;

// 非完整实现的 WebSocket 连接封装骨架（实际为原始 TCP 流）
class WsRawConnection : public std::enable_shared_from_this<WsRawConnection> {
public:
    explicit WsRawConnection(TcpSocket socket);

    void start();
    void send_text(const std::string& text);
    void send_binary(const std::vector<char>& data);
    void close();

    void set_message_callback(const WsMessageCallback& cb) { message_cb_ = cb; }
    void set_close_callback(const WsConnectionCallback& cb) { close_cb_ = cb; }

    int native_handle() const { return socket_.native_handle(); }

private:
    void run_read_loop();

    TcpSocket socket_;
    std::vector<char> read_buffer_;
    WsMessageCallback message_cb_;
    WsConnectionCallback close_cb_;
    std::atomic<bool> closed_{false};
    std::mutex send_mutex_;
};

} // namespace net
} // namespace chwell
