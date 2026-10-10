#include "chwell/discovery/consul_discovery.h"
#include <curl/curl.h>
#include <stdexcept>

namespace chwell { namespace discovery {
namespace {
constexpr std::size_t max_response_bytes = 4 * 1024 * 1024;
std::size_t receive_body(char* data, std::size_t size, std::size_t count, void* context) noexcept {
    auto& body = *static_cast<std::string*>(context);
    if (size && count > max_response_bytes / size) return 0;
    const auto bytes = size * count;
    if (bytes > max_response_bytes - body.size()) return 0;
    try { body.append(data, bytes); } catch (...) { return 0; }
    return bytes;
}
struct CurlHandle { CURL* value; ~CurlHandle() { if (value) curl_easy_cleanup(value); } };
struct CurlHeaders { curl_slist* value = nullptr; ~CurlHeaders() { curl_slist_free_all(value); }
    bool add(const std::string& text) {
        auto* result = curl_slist_append(value, text.c_str());
        if (!result) return false;
        value = result; return true;
    }
};
} // namespace

std::shared_ptr<ConsulServiceDiscovery> make_consul_discovery(const ConsulConfig& config) {
    auto endpoint = config.endpoint;
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    if ((endpoint.rfind("http://", 0) != 0 && endpoint.rfind("https://", 0) != 0) ||
        endpoint.find_first_of("?#@\r\n") != std::string::npos ||
        config.token.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument("Invalid Consul HTTP endpoint or token");
    }
    // Initialize once and deliberately retain curl globals until process exit.
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) throw std::runtime_error("libcurl initialization failed");
    auto transport = [config, endpoint](const std::string& method, const std::string& path, const std::string& body) {
        ConsulHttpResponse response;
        CurlHandle handle{curl_easy_init()};
        if (!handle.value) return response;
        CurlHeaders headers;
        if (!headers.add("Content-Type: application/json") ||
            (!config.token.empty() && !headers.add("X-Consul-Token: " + config.token))) return response;
        const auto url = endpoint + path;
        curl_easy_setopt(handle.value, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle.value, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(handle.value, CURLOPT_HTTPHEADER, headers.value);
        curl_easy_setopt(handle.value, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(handle.value, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        curl_easy_setopt(handle.value, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(config.connect_timeout_ms));
        curl_easy_setopt(handle.value, CURLOPT_TIMEOUT_MS, static_cast<long>(config.request_timeout_ms));
        curl_easy_setopt(handle.value, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle.value, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(handle.value, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle.value, CURLOPT_SSL_VERIFYHOST, 2L);
        if (!config.ca_file.empty()) curl_easy_setopt(handle.value, CURLOPT_CAINFO, config.ca_file.c_str());
        curl_easy_setopt(handle.value, CURLOPT_WRITEFUNCTION, receive_body);
        curl_easy_setopt(handle.value, CURLOPT_WRITEDATA, &response.body);
        response.transport_ok = curl_easy_perform(handle.value) == CURLE_OK;
        if (response.transport_ok) curl_easy_getinfo(handle.value, CURLINFO_RESPONSE_CODE, &response.status);
        return response;
    };
    return std::make_shared<ConsulServiceDiscovery>(config, std::move(transport));
}

} } // namespace chwell::discovery
