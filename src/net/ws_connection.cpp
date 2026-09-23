#include "chwell/net/ws_connection.h"
#include "chwell/core/logger.h"
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <string>

namespace chwell {
namespace net {

namespace {

// Minimal SHA-1 (for Sec-WebSocket-Accept)
struct Sha1 {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t total = 0;
    unsigned char buf[64];
    size_t buf_len = 0;

    static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void process(const unsigned char* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const void* data, size_t n) {
        total += n;
        const unsigned char* p = static_cast<const unsigned char*>(data);
        while (n > 0) {
            size_t take = 64 - buf_len;
            if (take > n) take = n;
            std::memcpy(buf + buf_len, p, take);
            buf_len += take;
            p += take;
            n -= take;
            if (buf_len == 64) {
                process(buf);
                buf_len = 0;
            }
        }
    }

    void final(unsigned char out[20]) {
        uint64_t bits = total * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char z = 0;
        while (buf_len != 56) update(&z, 1);
        unsigned char lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = static_cast<unsigned char>((bits >> (56 - i * 8)) & 0xFF);
        update(lenb, 8);
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = static_cast<unsigned char>((h[i] >> 24) & 0xFF);
            out[i * 4 + 1] = static_cast<unsigned char>((h[i] >> 16) & 0xFF);
            out[i * 4 + 2] = static_cast<unsigned char>((h[i] >> 8) & 0xFF);
            out[i * 4 + 3] = static_cast<unsigned char>(h[i] & 0xFF);
        }
    }
};

std::string base64(const unsigned char* data, size_t n) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned int v = data[i] << 16;
        if (i + 1 < n) v |= data[i + 1] << 8;
        if (i + 2 < n) v |= data[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? tbl[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? tbl[v & 63] : '=');
    }
    return out;
}

std::string ws_accept_key(const std::string& client_key) {
    std::string src = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 sha;
    sha.update(src.data(), src.size());
    unsigned char dig[20];
    sha.final(dig);
    return base64(dig, 20);
}

bool send_all_fd(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

WsRawConnection::WsRawConnection(TcpSocket socket)
    : socket_(std::move(socket)), read_buffer_(4096) {
}

void WsRawConnection::start() {
    auto self = shared_from_this();
    int fd = socket_.native_handle();

    // --- RFC6455 server handshake ---
    std::string req;
    char tmp[1024];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16 * 1024) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            closed_ = true;
            if (close_cb_) close_cb_(self);
            return;
        }
        req.append(tmp, static_cast<size_t>(n));
    }

    std::string key;
    {
        std::string lower = req;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        size_t pos = lower.find("sec-websocket-key:");
        if (pos != std::string::npos) {
            size_t vstart = pos + 18;
            while (vstart < req.size() && (req[vstart] == ' ' || req[vstart] == '\t')) ++vstart;
            size_t vend = req.find("\r\n", vstart);
            if (vend != std::string::npos) {
                key = req.substr(vstart, vend - vstart);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            }
        }
    }
    if (key.empty()) {
        const char* resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        send_all_fd(fd, resp, std::strlen(resp));
        closed_ = true;
        if (close_cb_) close_cb_(self);
        return;
    }

    std::string accept = ws_accept_key(key);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    if (!send_all_fd(fd, resp.data(), resp.size())) {
        closed_ = true;
        if (close_cb_) close_cb_(self);
        return;
    }

    run_read_loop();
}

void WsRawConnection::run_read_loop() {
    auto self = shared_from_this();
    std::string acc;

    while (!closed_ && socket_.is_open()) {
        ssize_t n = socket_.read(read_buffer_.data(), read_buffer_.size());
        if (n <= 0) break;
        acc.append(read_buffer_.data(), static_cast<size_t>(n));

        while (acc.size() >= 2) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(acc.data());
            bool fin = (p[0] & 0x80) != 0;
            unsigned char opcode = p[0] & 0x0F;
            bool masked = (p[1] & 0x80) != 0;
            uint64_t plen = p[1] & 0x7F;
            size_t hdr = 2;
            if (plen == 126) {
                if (acc.size() < 4) break;
                plen = (uint64_t(static_cast<unsigned char>(acc[2])) << 8) |
                       uint64_t(static_cast<unsigned char>(acc[3]));
                hdr = 4;
            } else if (plen == 127) {
                if (acc.size() < 10) break;
                plen = 0;
                for (int i = 0; i < 8; ++i) {
                    plen = (plen << 8) | uint64_t(static_cast<unsigned char>(acc[2 + i]));
                }
                hdr = 10;
            }
            if (plen > 4 * 1024 * 1024) {
                closed_ = true;
                break;
            }
            size_t mask_len = masked ? 4 : 0;
            if (acc.size() < hdr + mask_len + plen) break;

            std::string payload = acc.substr(hdr + mask_len, static_cast<size_t>(plen));
            if (masked) {
                unsigned char mk[4];
                for (int i = 0; i < 4; ++i) mk[i] = static_cast<unsigned char>(acc[hdr + i]);
                for (size_t i = 0; i < payload.size(); ++i) {
                    payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mk[i % 4]);
                }
            }
            acc.erase(0, hdr + mask_len + static_cast<size_t>(plen));

            if (opcode == 0x8) { // close
                closed_ = true;
                break;
            }
            if (opcode == 0x9) { // ping -> pong
                std::string pong;
                pong.push_back(static_cast<char>(0x8A));
                if (payload.size() < 126) {
                    pong.push_back(static_cast<char>(payload.size()));
                    pong += payload;
                }
                std::lock_guard<std::mutex> lock(send_mutex_);
                send_all_fd(socket_.native_handle(), pong.data(), pong.size());
                continue;
            }
            if (opcode == 0xA) continue; // pong

            if ((opcode == 0x1 || opcode == 0x2 || opcode == 0x0) && fin) {
                if (message_cb_) message_cb_(self, payload);
            }
        }
    }

    closed_ = true;
    if (close_cb_) close_cb_(self);
}

void WsRawConnection::send_text(const std::string& text) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (closed_ || !socket_.is_open()) return;
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    size_t len = text.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
        }
    }
    frame += text;
    if (!send_all_fd(socket_.native_handle(), frame.data(), frame.size())) {
        ErrorCode ec;
        socket_.shutdown(SHUT_RDWR, ec);
        socket_.close(ec);
        closed_ = true;
    }
}

void WsRawConnection::send_binary(const std::vector<char>& data) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (closed_ || !socket_.is_open()) return;
    std::string frame;
    frame.push_back(static_cast<char>(0x82));
    size_t len = data.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
        }
    }
    frame.append(data.data(), data.size());
    if (!send_all_fd(socket_.native_handle(), frame.data(), frame.size())) {
        ErrorCode ec;
        socket_.shutdown(SHUT_RDWR, ec);
        socket_.close(ec);
        closed_ = true;
    }
}

void WsRawConnection::close() {
    closed_ = true;
    std::lock_guard<std::mutex> lock(send_mutex_);
    ErrorCode ec;
    socket_.shutdown(SHUT_RDWR, ec);
    socket_.close(ec);
}

} // namespace net
} // namespace chwell
