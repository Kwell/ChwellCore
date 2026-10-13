#include "chwell/http/http_client.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace chwell::http;
using Clock = std::chrono::steady_clock;
namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
ClientResponse sentinel() { return {987, {{"old", "header"}}, {{"old", "trailer"}}, "unchanged"}; }
bool unchanged(const ClientResponse& out) {
    const auto expected = sentinel();
    return out.status == expected.status && out.headers == expected.headers &&
        out.trailers == expected.trailers && out.body == expected.body;
}
std::size_t count(const ClientHeaders& headers, const std::string& name) {
    return static_cast<std::size_t>(std::count_if(headers.begin(), headers.end(),
        [&](const auto& entry) { return entry.first == name; }));
}
void failure(HttpClient& client, const ClientRequest& request, ClientStatus expected,
             const ClientOptions& options = {}, const CancellationToken& cancellation = {}) {
    auto out = sentinel();
    const auto result = client.request(request, out, options, cancellation);
    if (result.status != expected) {
        std::cerr << "Expected status " << static_cast<int>(expected) << ", got "
                  << static_cast<int>(result.status) << " (native " << result.native_code << ")\n";
    }
    check(result.status == expected, "unexpected failure category");
    check(unchanged(out), "failure replaced response");
}
}
int main(int argc, char** argv) try {
    if (argc != 5) throw std::runtime_error("Expected HTTP base, HTTPS base, CA file and stalled TLS URL");
    const std::string base = argv[1], secure = argv[2];
    HttpClient client; // Initialize before any workers.
    ClientRequest request;
    request.url = base + "/binary";
    ClientResponse response;
    check(client.request(request, response).ok() && response.status == 200 &&
          response.body == std::string("hello\0world", 11), "binary GET failed");
    check(count(response.headers, "x-repeat") == 2, "lost duplicate response header");
    check(count(response.headers, "x-empty") == 1, "lost empty header");
    for (const auto* method : {"POST", "PUT", "PATCH", "DELETE", "OPTIONS"}) {
        request = {method, base + "/echo", {{"X-Empty", ""}}, std::string("abc\0xyz", 7)};
        check(client.request(request, response).ok() &&
              response.body == std::string(method) + ":" + request.body, "method/body changed");
        check(count(response.headers, "x-received-empty") == 1, "empty request header missing");
    }
    request = {"GET", base + "/chunked", {}, {}};
    check(client.request(request, response).ok() && response.body == std::string("a\0b", 3), "chunked binary failed");
    check(response.trailers == ClientHeaders{{"x-checksum", "complete"}}, "lost trailer");
    check(count(response.headers, "x-checksum") == 0, "trailer mixed with headers");
    request.url = base + "/interim";
    check(client.request(request, response).ok() && count(response.headers, "x-interim") == 0 &&
          count(response.headers, "x-final") == 1, "interim headers mixed with final response");
    request.url = base + "/redirect";
    check(client.request(request, response).ok() && response.status == 302, "redirect followed");
    request.url = base + "/unavailable";
    check(client.request(request, response).ok() && response.status == 503 && response.body == "unavailable",
          "HTTP error lost its response");
    request.url = base + "/compressed";
    check(client.request(request, response).ok() && response.body == std::string(4096, 'x'),
          "compressed response was not decoded");

    ClientOptions limited;
    limited.max_response_bytes = 3;
    request.url = base + "/binary";
    failure(client, request, ClientStatus::ResponseTooLarge, limited);
    request.url = base + "/compressed";
    limited.max_response_bytes = 100; // Gzip fits on the wire; its 4096 decoded bytes exceed the budget.
    failure(client, request, ClientStatus::ResponseTooLarge, limited);
    limited = {};
    limited.max_header_bytes = 64;
    request.url = base + "/interim";
    failure(client, request, ClientStatus::ResponseTooLarge, limited);
    request.url = base + "/large-header";
    failure(client, request, ClientStatus::ResponseTooLarge, limited);
    request.url = base + "/chunked";
    limited.max_header_bytes = 80; // Initial headers fit; trailer exceeds the combined budget.
    failure(client, request, ClientStatus::ResponseTooLarge, limited);
    request = {"HEAD", base + "/head", {}, {}};
    limited = {};
    limited.max_response_bytes = 0;
    check(client.request(request, response, limited).ok() && response.body.empty(), "HEAD consumed advertised body");

    for (const auto* path : {"/slow-header", "/slow-body", "/drip"}) {
        request = {"GET", base + path, {}, {}};
        limited = {};
        limited.request_timeout_ms = 180;
        const auto started = Clock::now();
        failure(client, request, ClientStatus::TimedOut, limited);
        check(Clock::now() - started < std::chrono::seconds(2), "overall timeout extended by progress");
    }
    request = {"POST", base + "/no-read", {}, std::string(16 * 1024 * 1024, 'x')};
    limited = {};
    limited.max_request_bytes = request.body.size();
    limited.request_timeout_ms = 180;
    failure(client, request, ClientStatus::TimedOut, limited);

    request = {"GET", base + "/pre-cancelled", {}, {}};
    CancellationToken cancelled;
    auto copied = cancelled;
    copied.cancel();
    check(cancelled.cancelled(), "token copies do not share cancellation");
    failure(client, request, ClientStatus::Cancelled, {}, cancelled);
    request.url = base + "/slow-header";
    CancellationToken live;
    auto out = sentinel();
    auto pending = std::async(std::launch::async, [&] { return client.request(request, out, {}, live); });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto cancelled_at = Clock::now();
    live.cancel();
    check(pending.get().status == ClientStatus::Cancelled && unchanged(out), "cross-thread cancellation failed");
    check(Clock::now() - cancelled_at < std::chrono::seconds(2), "cancel failed to interrupt stalled response");

    // Confirm the server committed before cancelling its delayed response.
    request = {"POST", base + "/mutate", {}, "increment"};
    CancellationToken mutation;
    out = sentinel();
    auto mutation_call = std::async(std::launch::async, [&] { return client.request(request, out, {}, mutation); });
    bool committed = false;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline) {
        ClientResponse stats;
        if (client.request({"GET", base + "/stats", {}, {}}, stats).ok() && stats.body == "1") {
            committed = true; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    mutation.cancel();
    check(mutation_call.get().status == ClientStatus::Cancelled && unchanged(out), "mutation cancellation failed");
    check(committed, "mutation was not observed before cancellation");

    std::vector<std::future<bool>> workers;
    for (int i = 0; i < 8; ++i) workers.push_back(std::async(std::launch::async, [&] {
        ClientResponse local;
        return client.request({"GET", base + "/binary", {}, {}}, local).ok() && local.body.size() == 11;
    }));
    for (auto& worker : workers) check(worker.get(), "concurrent request interfered");

    request = {"GET", base + "/binary", {}, {}};
    limited = {};
    limited.connect_timeout_ms = 0;
    failure(client, request, ClientStatus::Invalid, limited);
    limited = {};
    limited.request_timeout_ms = -1;
    failure(client, request, ClientStatus::Invalid, limited);
    limited = {};
    limited.max_response_bytes = 64 * 1024 * 1024 + 1;
    failure(client, request, ClientStatus::Invalid, limited);
    for (const auto* url : {"file:///etc/passwd", "http://user:password@localhost/", "http://localhost/#fragment", "http://localhost/\r\nInjected"}) {
        request.url = url;
        failure(client, request, ClientStatus::Invalid);
    }
    request.url = base + std::string("/\0hidden", 8);
    failure(client, request, ClientStatus::Invalid);
    request.url = base + "/binary";
    for (const auto& field : ClientHeaders{{"Host", "other"}, {"Content-Length", "0"}, {"Transfer-Encoding", "chunked"},
            {"Connection", "keep-alive"}, {"Proxy-Authorization", "secret"}, {"Expect", "100-continue"},
            {"Bad Name", "x"}, {"X-Test", "value\r\nInjected: true"}, {"X-Test", std::string("a\0b", 3)}}) {
        request.headers = {field};
        failure(client, request, ClientStatus::Invalid);
    }
    request.headers.clear(); request.method = "TRACE";
    failure(client, request, ClientStatus::Invalid);
    request.method = "GET"; request.body = "body";
    failure(client, request, ClientStatus::Invalid);
    request.method = "POST";
    limited = {}; limited.max_request_bytes = 3;
    failure(client, request, ClientStatus::Invalid, limited);
    request = {"GET", base + "/disconnect", {}, {}};
    failure(client, request, ClientStatus::TransportError);
    request.url = base + "/partial";
    failure(client, request, ClientStatus::TransportError);

    request.url = secure + "/binary";
    failure(client, request, ClientStatus::TlsError);
    limited = {}; limited.ca_file = argv[3];
    check(client.request(request, response, limited).ok() && response.body.size() == 11, "custom trusted CA failed");
    request.url.replace(request.url.find("localhost"), 9, "127.0.0.1");
    failure(client, request, ClientStatus::TlsError, limited);
    request.url = argv[4];
    limited = {};
    limited.connect_timeout_ms = 180;
    limited.request_timeout_ms = 2500;
    const auto connecting = Clock::now();
    failure(client, request, ClientStatus::TimedOut, limited);
    check(Clock::now() - connecting < std::chrono::seconds(2), "TLS handshake exceeded connect budget");
    std::cout << "PASS: " << checks << " HTTP/TLS contract checks\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
