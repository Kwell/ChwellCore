#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace chwell { namespace http {

// Copies share a one-way cancellation flag. Create a new token for a new intent.
class CancellationToken {
public:
    CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    void cancel() const noexcept { if (flag_) flag_->store(true, std::memory_order_relaxed); }
    bool cancelled() const noexcept { return flag_ && flag_->load(std::memory_order_relaxed); }
private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

using ClientHeaders = std::vector<std::pair<std::string, std::string>>;
struct ClientRequest {
    std::string method = "GET";
    std::string url;
    ClientHeaders headers;
    std::string body; // Exact bytes, including NUL. GET/HEAD require an empty body.
};
struct ClientOptions {
    int connect_timeout_ms = 1000;
    int request_timeout_ms = 3000; // Overall steady-clock budget from request entry.
    std::size_t max_request_bytes = 1024 * 1024;
    std::size_t max_response_bytes = 4 * 1024 * 1024;
    std::size_t max_header_bytes = 64 * 1024; // Includes interim responses and trailers.
    std::string ca_file; // Empty uses the TLS backend's trust store; verification stays enabled.
};
struct ClientResponse {
    long status = 0;
    ClientHeaders headers; // Final response; duplicates retained, names normalized to lowercase.
    ClientHeaders trailers;
    std::string body;
};
enum class ClientStatus { Ok, Invalid, Cancelled, TimedOut, ResponseTooLarge, TlsError, TransportError, Unsupported };
struct ClientResult {
    ClientStatus status = ClientStatus::TransportError;
    int native_code = 0; // libcurl error code where available, never a URL/credential dump.
    bool ok() const { return status == ClientStatus::Ok; }
};

// Optional Chwell::http_client component (libcurl >=7.68, asynchronous DNS).
// Construct before worker threads on libcurl builds without thread-safe global init.
// Blocking API: use a worker, not the application IO thread. Concurrent requests
// use independent handles. No pooling, automatic redirects, proxies or retries.
class HttpClient {
public:
    HttpClient();
    // A complete HTTP response, including 3xx/4xx/5xx, is Ok. Every failure leaves
    // out unchanged. Cancellation/timeout do NOT prove a remote mutation failed.
    // Input/options must remain unchanged for the duration of this call.
    ClientResult request(const ClientRequest& input, ClientResponse& out,
                         const ClientOptions& options = {}, const CancellationToken& cancellation = {}) const;
private:
    bool initialized_ = false, asynchronous_dns_ = false;
};

} } // namespace chwell::http
