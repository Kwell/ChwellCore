#include "chwell/discovery/consul_discovery.h"
#include "chwell/http/http_client.h"
#include <stdexcept>

namespace chwell { namespace discovery {

std::shared_ptr<ConsulServiceDiscovery> make_consul_discovery(const ConsulConfig& config) {
    auto endpoint = config.endpoint;
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    if ((endpoint.rfind("http://", 0) != 0 && endpoint.rfind("https://", 0) != 0) ||
        endpoint.find_first_of("?#@\r\n") != std::string::npos ||
        endpoint.find('\0') != std::string::npos || config.token.find('\0') != std::string::npos ||
        config.token.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument("Invalid Consul HTTP endpoint or token");
    }
    auto client = std::make_shared<http::HttpClient>();
    auto transport = [config, endpoint, client](const std::string& method, const std::string& path, const std::string& body) {
        http::ClientRequest request;
        request.method = method;
        request.url = endpoint + path;
        request.body = body;
        request.headers.emplace_back("Content-Type", "application/json");
        if (!config.token.empty()) request.headers.emplace_back("X-Consul-Token", config.token);
        http::ClientOptions options;
        options.connect_timeout_ms = config.connect_timeout_ms;
        options.request_timeout_ms = config.request_timeout_ms;
        options.ca_file = config.ca_file;
        http::ClientResponse response;
        if (!client->request(request, response, options).ok()) return ConsulHttpResponse{};
        return ConsulHttpResponse{true, response.status, std::move(response.body)};
    };
    return std::make_shared<ConsulServiceDiscovery>(config, std::move(transport));
}

} } // namespace chwell::discovery
