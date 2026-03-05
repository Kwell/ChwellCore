#pragma once

#include "chwell/net/posix_io.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace net {

// 简单的 TLS 封装：基于 OpenSSL，对现有 TcpSocket 做包装。
// 仅在定义 CHWELL_USE_OPENSSL 时提供真实实现，否则为空壳实现。

class TlsContext {
public:
    TlsContext();
    ~TlsContext();

    // 初始化为服务端上下文，加载证书和私钥
    bool init_server(const std::string& cert_file,
                     const std::string& key_file);

    // 初始化为客户端上下文，可选 CA 证书（为空则使用系统默认）
    bool init_client(const std::string& ca_file = std::string());

    bool is_server() const { return is_server_; }

private:
    friend class TlsConnection;

    void* ctx_;      // 实际类型为 SSL_CTX*
    bool is_server_;
};

class TlsConnection {
public:
    TlsConnection(TlsContext& ctx, TcpSocket&& socket);
    ~TlsConnection();

    // 进行 TLS 握手，成功返回 true
    bool handshake();

    // 阻塞读/写，与 TcpSocket 类似
    ssize_t read(void* buf, std::size_t len);
    ssize_t write(const void* buf, std::size_t len);

    void close();

    int native_handle() const { return socket_.native_handle(); }

private:
    TlsContext& ctx_;
    TcpSocket socket_;
    void* ssl_;  // 实际类型为 SSL*
    bool established_;
};

} // namespace net
} // namespace chwell

