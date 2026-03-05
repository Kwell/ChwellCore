#include "chwell/net/tls.h"

#ifdef CHWELL_USE_OPENSSL

#include <openssl/ssl.h>
#include <openssl/err.h>

namespace chwell {
namespace net {

namespace {

inline void log_ssl_error(const std::string& prefix) {
    unsigned long err = ERR_get_error();
    if (err != 0) {
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        CHWELL_LOG_ERROR(prefix << ": " << buf);
    } else {
        CHWELL_LOG_ERROR(prefix);
    }
}

} // anonymous namespace

TlsContext::TlsContext()
    : ctx_(nullptr), is_server_(false) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
}

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX* real = static_cast<SSL_CTX*>(ctx_);
        SSL_CTX_free(real);
        ctx_ = nullptr;
    }
}

bool TlsContext::init_server(const std::string& cert_file,
                             const std::string& key_file) {
    is_server_ = true;
    SSL_CTX* c = SSL_CTX_new(TLS_server_method());
    if (!c) {
        log_ssl_error("TLS: SSL_CTX_new server failed");
        return false;
    }
    if (SSL_CTX_use_certificate_file(c, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        log_ssl_error("TLS: use_certificate_file failed");
        SSL_CTX_free(c);
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(c, key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        log_ssl_error("TLS: use_PrivateKey_file failed");
        SSL_CTX_free(c);
        return false;
    }
    if (!SSL_CTX_check_private_key(c)) {
        log_ssl_error("TLS: check_private_key failed");
        SSL_CTX_free(c);
        return false;
    }
    ctx_ = c;
    return true;
}

bool TlsContext::init_client(const std::string& ca_file) {
    is_server_ = false;
    SSL_CTX* c = SSL_CTX_new(TLS_client_method());
    if (!c) {
        log_ssl_error("TLS: SSL_CTX_new client failed");
        return false;
    }
    if (!ca_file.empty()) {
        if (!SSL_CTX_load_verify_locations(c, ca_file.c_str(), nullptr)) {
            log_ssl_error("TLS: load_verify_locations failed");
            SSL_CTX_free(c);
            return false;
        }
        SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
    }
    ctx_ = c;
    return true;
}

TlsConnection::TlsConnection(TlsContext& ctx, TcpSocket&& socket)
    : ctx_(ctx),
      socket_(std::move(socket)),
      ssl_(nullptr),
      established_(false) {
    SSL_CTX* real_ctx = static_cast<SSL_CTX*>(ctx_.ctx_);
    SSL* s = SSL_new(real_ctx);
    if (!s) {
        log_ssl_error("TLS: SSL_new failed");
        return;
    }
    SSL_set_fd(s, socket_.native_handle());
    ssl_ = s;
}

TlsConnection::~TlsConnection() {
    close();
}

bool TlsConnection::handshake() {
    if (!ssl_) return false;
    int ret = ctx_.is_server() ? SSL_accept(static_cast<SSL*>(ssl_))
                               : SSL_connect(static_cast<SSL*>(ssl_));
    if (ret <= 0) {
        log_ssl_error("TLS: handshake failed");
        return false;
    }
    established_ = true;
    return true;
}

ssize_t TlsConnection::read(void* buf, std::size_t len) {
    if (!ssl_) return -1;
    int n = SSL_read(static_cast<SSL*>(ssl_), buf, static_cast<int>(len));
    if (n <= 0) {
        int err = SSL_get_error(static_cast<SSL*>(ssl_), n);
        if (err != SSL_ERROR_ZERO_RETURN && err != SSL_ERROR_WANT_READ &&
            err != SSL_ERROR_WANT_WRITE) {
            log_ssl_error("TLS: read error");
        }
        return -1;
    }
    return static_cast<ssize_t>(n);
}

ssize_t TlsConnection::write(const void* buf, std::size_t len) {
    if (!ssl_) return -1;
    int n = SSL_write(static_cast<SSL*>(ssl_), buf, static_cast<int>(len));
    if (n <= 0) {
        int err = SSL_get_error(static_cast<SSL*>(ssl_), n);
        if (err != SSL_ERROR_ZERO_RETURN && err != SSL_ERROR_WANT_READ &&
            err != SSL_ERROR_WANT_WRITE) {
            log_ssl_error("TLS: write error");
        }
        return -1;
    }
    return static_cast<ssize_t>(n);
}

void TlsConnection::close() {
    if (ssl_) {
        SSL* s = static_cast<SSL*>(ssl_);
        SSL_shutdown(s);
        SSL_free(s);
        ssl_ = nullptr;
    }
    if (socket_.is_open()) {
        ErrorCode ec;
        socket_.shutdown(SHUT_RDWR, ec);
        socket_.close(ec);
    }
    established_ = false;
}

} // namespace net
} // namespace chwell

#else  // CHWELL_USE_OPENSSL

namespace chwell {
namespace net {

TlsContext::TlsContext() : ctx_(nullptr), is_server_(false) {}
TlsContext::~TlsContext() {}

bool TlsContext::init_server(const std::string&, const std::string&) {
    CHWELL_LOG_ERROR("TLS not available: rebuild with CHWELL_USE_OPENSSL=ON");
    return false;
}

bool TlsContext::init_client(const std::string&) {
    CHWELL_LOG_ERROR("TLS not available: rebuild with CHWELL_USE_OPENSSL=ON");
    return false;
}

TlsConnection::TlsConnection(TlsContext& ctx, TcpSocket&& socket)
    : ctx_(ctx), socket_(std::move(socket)), ssl_(nullptr), established_(false) {
}

TlsConnection::~TlsConnection() {
    close();
}

bool TlsConnection::handshake() {
    CHWELL_LOG_ERROR("TLS not available: rebuild with CHWELL_USE_OPENSSL=ON");
    return false;
}

ssize_t TlsConnection::read(void*, std::size_t) {
    CHWELL_LOG_ERROR("TLS not available: rebuild with CHWELL_USE_OPENSSL=ON");
    return -1;
}

ssize_t TlsConnection::write(const void*, std::size_t) {
    CHWELL_LOG_ERROR("TLS not available: rebuild with CHWELL_USE_OPENSSL=ON");
    return -1;
}

void TlsConnection::close() {
    if (socket_.is_open()) {
        ErrorCode ec;
        socket_.shutdown(SHUT_RDWR, ec);
        socket_.close(ec);
    }
    established_ = false;
}

} // namespace net
} // namespace chwell

#endif // CHWELL_USE_OPENSSL

