#include "chwell/http/http_client.h"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>

namespace chwell { namespace http {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_buffer = 64 * 1024 * 1024;
struct Easy { CURL* value = curl_easy_init(); ~Easy() { if (value) curl_easy_cleanup(value); } };
struct Multi {
    CURLM* value = curl_multi_init();
    CURL* attached = nullptr;
    ~Multi() {
        if (value && attached) curl_multi_remove_handle(value, attached);
        if (value) curl_multi_cleanup(value);
    }
};
struct Headers {
    curl_slist* value = nullptr;
    ~Headers() { curl_slist_free_all(value); }
    bool add(const std::string& text) {
        auto* pending = curl_slist_append(value, text.c_str());
        if (!pending) return false;
        value = pending; return true;
    }
};
struct Url {
    CURLU* value = curl_url();
    ~Url() { if (value) curl_url_cleanup(value); }
    std::string part(CURLUPart field) const {
        char* text = nullptr;
        if (curl_url_get(value, field, &text, 0) != CURLUE_OK) return {};
        std::unique_ptr<char, decltype(&curl_free)> owned(text, curl_free);
        return std::string(text);
    }
    bool has(CURLUPart field) const {
        char* text = nullptr;
        const bool present = curl_url_get(value, field, &text, 0) == CURLUE_OK;
        curl_free(text); return present;
    }
};
std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}
bool token(std::string_view text) {
    if (text.empty()) return false;
    for (unsigned char c : text) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) continue;
        if (std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) == std::string_view::npos) return false;
    }
    return true;
}
bool field_value(std::string_view text) {
    for (unsigned char c : text) if ((c < 32 && c != '\t') || c == 127) return false;
    return true;
}
bool valid(const ClientRequest& input, const ClientOptions& options) {
    if (options.connect_timeout_ms <= 0 || options.request_timeout_ms <= 0 ||
        options.max_request_bytes > max_buffer || options.max_response_bytes > max_buffer ||
        options.max_header_bytes > max_buffer || input.body.size() > options.max_request_bytes ||
        options.ca_file.size() > 4096 || options.ca_file.find('\0') != std::string::npos ||
        input.url.empty() || input.url.size() > 8192 || input.headers.size() > 64) return false;
    for (unsigned char c : input.url) if (c <= 32 || c == 127) return false;
    if (input.method != "GET" && input.method != "HEAD" && input.method != "POST" && input.method != "PUT" &&
        input.method != "PATCH" && input.method != "DELETE" && input.method != "OPTIONS") return false;
    if ((input.method == "GET" || input.method == "HEAD") && !input.body.empty()) return false;
    std::size_t bytes = 0;
    for (const auto& header : input.headers) {
        if (header.first.size() > 256 || header.second.size() > 8192 || !token(header.first) || !field_value(header.second)) return false;
        bytes += header.first.size() + header.second.size() + 4;
        if (bytes > 65536) return false;
        const auto name = lower(header.first);
        if (name == "host" || name == "content-length" || name == "transfer-encoding" || name == "connection" ||
            name == "proxy-authorization" || name == "expect") return false;
    }
    Url url;
    if (!url.value || curl_url_set(url.value, CURLUPART_URL, input.url.c_str(), 0) != CURLUE_OK) return false;
    const auto scheme = lower(url.part(CURLUPART_SCHEME));
    return (scheme == "http" || scheme == "https") && !url.part(CURLUPART_HOST).empty() &&
        !url.has(CURLUPART_USER) && !url.has(CURLUPART_PASSWORD) && !url.has(CURLUPART_FRAGMENT);
}
struct Context {
    ClientResponse pending;
    const ClientOptions& options;
    const CancellationToken& cancellation;
    Clock::time_point deadline;
    ClientStatus failure = ClientStatus::Ok;
    std::size_t header_bytes = 0;
    bool headers_complete = false;
    bool abort() noexcept {
        if (failure != ClientStatus::Ok) return true;
        if (cancellation.cancelled()) failure = ClientStatus::Cancelled;
        else if (Clock::now() >= deadline) failure = ClientStatus::TimedOut;
        return failure != ClientStatus::Ok;
    }
};
std::size_t body(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    if (context.abort()) return 0;
    if (size && count > (std::numeric_limits<std::size_t>::max)() / size) return 0;
    const auto bytes = size * count;
    if (bytes > context.options.max_response_bytes - context.pending.body.size()) {
        context.failure = ClientStatus::ResponseTooLarge; return 0;
    }
    try { context.pending.body.append(data, bytes); }
    catch (...) { context.failure = ClientStatus::TransportError; return 0; }
    return bytes;
}
std::size_t header(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    if (context.abort()) return 0;
    if (size && count > (std::numeric_limits<std::size_t>::max)() / size) return 0;
    const auto bytes = size * count;
    if (bytes > context.options.max_header_bytes - context.header_bytes) {
        context.failure = ClientStatus::ResponseTooLarge; return 0;
    }
    context.header_bytes += bytes;
    try {
        std::string_view line(data, bytes);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
        if (line.substr(0, 5) == "HTTP/") {
            context.pending.headers.clear(); context.pending.trailers.clear(); context.headers_complete = false;
        } else if (line.empty()) context.headers_complete = true;
        else {
            const auto colon = line.find(':');
            if (colon == std::string_view::npos || !token(line.substr(0, colon))) {
                context.failure = ClientStatus::TransportError; return 0;
            }
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
            if (!field_value(value)) { context.failure = ClientStatus::TransportError; return 0; }
            (context.headers_complete ? context.pending.trailers : context.pending.headers).emplace_back(
                lower(line.substr(0, colon)), std::string(value));
        }
    } catch (...) { context.failure = ClientStatus::TransportError; return 0; }
    return bytes;
}
int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<Context*>(opaque)->abort() ? 1 : 0;
}
long remaining(Clock::time_point deadline) {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<long>(std::max<std::int64_t>(1, value)); // Zero would disable libcurl's timeout.
}
ClientStatus classify(CURLcode code) {
    if (code == CURLE_OPERATION_TIMEDOUT) return ClientStatus::TimedOut;
    if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE ||
        code == CURLE_SSL_CONNECT_ERROR || code == CURLE_SSL_CERTPROBLEM || code == CURLE_SSL_CIPHER)
        return ClientStatus::TlsError;
    return ClientStatus::TransportError;
}
} // namespace

HttpClient::HttpClient() {
    // Shared initialization; globals outlive requests and detached resolver work.
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    initialized_ = initialized == CURLE_OK;
    if (initialized_) {
        const auto* version = curl_version_info(CURLVERSION_NOW);
        asynchronous_dns_ = version && (version->features & CURL_VERSION_ASYNCHDNS);
    }
}
ClientResult HttpClient::request(const ClientRequest& input, ClientResponse& out,
    const ClientOptions& options, const CancellationToken& cancellation) const try {
    const auto started = Clock::now();
    if (!initialized_) return {ClientStatus::TransportError, CURLE_FAILED_INIT};
    if (!asynchronous_dns_) return {ClientStatus::Unsupported};
    if (!valid(input, options)) return {ClientStatus::Invalid};
    Context context{{}, options, cancellation, started + std::chrono::milliseconds(options.request_timeout_ms)};
    if (context.abort()) return {context.failure};
    Headers headers;
    if (!headers.add("Expect:")) return {ClientStatus::TransportError, CURLE_OUT_OF_MEMORY};
    for (const auto& value : input.headers)
        if (!headers.add(value.first + (value.second.empty() ? ";" : ": " + value.second)))
            return {ClientStatus::TransportError, CURLE_OUT_OF_MEMORY};
    Easy easy;
    Multi multi;
    if (!easy.value || !multi.value) return {ClientStatus::TransportError, CURLE_OUT_OF_MEMORY};
    CURLcode configured = CURLE_OK;
    const auto set = [&](CURLoption option, auto value) {
        if (configured == CURLE_OK) configured = curl_easy_setopt(easy.value, option, value);
    };
    set(CURLOPT_URL, input.url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    set(CURLOPT_PROTOCOLS_STR, "http,https");
#else
    set(CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    set(CURLOPT_HTTPHEADER, headers.value);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_PROXY, ""); // Ignore environment proxies; do not silently change the network path.
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_FRESH_CONNECT, 1L);
    set(CURLOPT_FORBID_REUSE, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    if (!options.ca_file.empty()) set(CURLOPT_CAINFO, options.ca_file.c_str());
    set(CURLOPT_ACCEPT_ENCODING, ""); // Limit decoded bodies, including supported compression.
    set(CURLOPT_CONNECTTIMEOUT_MS, std::min<long>(options.connect_timeout_ms, remaining(context.deadline)));
    set(CURLOPT_TIMEOUT_MS, remaining(context.deadline));
    set(CURLOPT_WRITEFUNCTION, body); set(CURLOPT_WRITEDATA, &context);
    set(CURLOPT_HEADERFUNCTION, header); set(CURLOPT_HEADERDATA, &context);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, progress); set(CURLOPT_XFERINFODATA, &context);
    if (input.method == "HEAD") set(CURLOPT_NOBODY, 1L);
    else if (input.method != "GET") {
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(input.body.size()));
        set(CURLOPT_POSTFIELDS, input.body.data());
    }
    set(CURLOPT_CUSTOMREQUEST, input.method.c_str());
    if (configured != CURLE_OK) return {ClientStatus::TransportError, configured};
    if (curl_multi_add_handle(multi.value, easy.value) != CURLM_OK) return {ClientStatus::TransportError};
    multi.attached = easy.value;
    CURLcode completed = CURLE_OK;
    bool done = false;
    while (!done) {
        if (context.abort()) return {context.failure};
        int running = 0;
        if (curl_multi_perform(multi.value, &running) != CURLM_OK) return {ClientStatus::TransportError};
        int queued = 0;
        while (auto* message = curl_multi_info_read(multi.value, &queued))
            if (message->msg == CURLMSG_DONE) { completed = message->data.result; done = true; }
        if (done) break;
        if (!running) return {ClientStatus::TransportError};
        if (curl_multi_poll(multi.value, nullptr, 0, static_cast<int>(std::min<long>(50, remaining(context.deadline))), nullptr) != CURLM_OK)
            return {ClientStatus::TransportError};
    }
    if (context.abort()) return {context.failure, completed};
    if (completed != CURLE_OK) return {classify(completed), completed};
    if (curl_easy_getinfo(easy.value, CURLINFO_RESPONSE_CODE, &context.pending.status) != CURLE_OK || context.pending.status < 100)
        return {ClientStatus::TransportError};
    out = std::move(context.pending);
    return {ClientStatus::Ok};
} catch (...) {
    return {ClientStatus::TransportError};
}

} } // namespace chwell::http
