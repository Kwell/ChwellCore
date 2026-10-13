#include "operations.h"
#include <nlohmann/json.hpp>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace reference {
OperationsState::OperationsState()
    : requests(registry_.register_counter("chwell_requests_total", "Completed business requests")),
      errors(registry_.register_counter("chwell_request_errors_total", "Failed business requests including unknown outcomes")),
      rejected(registry_.register_counter("chwell_rejected_inputs_total", "Input chunks rejected at admission")),
      dropped(registry_.register_counter("chwell_drain_dropped_inputs_total", "Accepted input chunks discarded at drain deadline")),
      queued(registry_.register_gauge("chwell_queued_inputs", "Accepted input chunks waiting for the worker")),
      inflight(registry_.register_gauge("chwell_inflight_requests", "Business requests executing")),
      sessions(registry_.register_gauge("chwell_sessions", "Gateway local player leases")) {}
void OperationsState::main_progress() { std::lock_guard<std::mutex> lock(mutex_); main_ = Clock::now(); }
void OperationsState::worker_progress() { std::lock_guard<std::mutex> lock(mutex_); worker_ = Clock::now(); }
void OperationsState::started() { std::lock_guard<std::mutex> lock(mutex_); started_ = true; }
void OperationsState::dependencies(bool database, bool discovery, std::size_t nodes) {
    std::lock_guard<std::mutex> lock(mutex_);
    database_ = database; discovery_ = discovery; nodes_ = nodes; checked_ = Clock::now();
}
void OperationsState::drain() { std::lock_guard<std::mutex> lock(mutex_); draining_ = true; }
void OperationsState::finished() { std::lock_guard<std::mutex> lock(mutex_); finished_ = true; }
void OperationsState::stopped() { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
bool OperationsState::live_locked(Clock::time_point now) const {
    return !stopped_ && now - main_ < std::chrono::seconds(15) &&
        ((draining_ && finished_) || now - worker_ < std::chrono::seconds(15));
}
bool OperationsState::ready_locked(Clock::time_point now) const {
    return started_ && !draining_ && live_locked(now) && database_ && discovery_ && nodes_ > 0 &&
        now - checked_ < std::chrono::seconds(3);
}
bool OperationsState::live() const { std::lock_guard<std::mutex> lock(mutex_); return live_locked(Clock::now()); }
bool OperationsState::ready() const { std::lock_guard<std::mutex> lock(mutex_); return ready_locked(Clock::now()); }
bool OperationsState::complete() const { std::lock_guard<std::mutex> lock(mutex_); return finished_; }
std::string OperationsState::health(bool readiness) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    return nlohmann::json{{"ok", readiness ? ready_locked(now) : live_locked(now)},
        {"started", started_}, {"draining", draining_}, {"drained", finished_},
        {"database", database_}, {"discovery", discovery_}, {"nodes", nodes_}}.dump() + "\n";
}
std::string OperationsState::metrics() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = Clock::now();
        registry_.register_gauge("chwell_ready", "Cached dependency readiness").set(ready_locked(now));
        registry_.register_gauge("chwell_live", "Control and business worker progress").set(live_locked(now));
        registry_.register_gauge("chwell_draining", "Admission is closed for deployment drain").set(draining_);
        registry_.register_gauge("chwell_database_available", "Last database probe succeeded").set(database_);
        registry_.register_gauge("chwell_discovery_available", "Last discovery operation succeeded").set(discovery_);
        registry_.register_gauge("chwell_game_nodes", "Discovered nodes or this Game registration").set(nodes_);
    }
    return registry_.export_metrics();
}
namespace {
void nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("Cannot configure operations socket");
}
struct Peer {
    int fd;
    Clock::time_point deadline;
    std::string input, output;
    std::size_t sent = 0;
};
std::string response(int code, const std::string& body, const std::string& type = "application/json") {
    const std::string reason = code == 200 ? "OK" : code == 503 ? "Service Unavailable" : "Bad Request";
    return "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + type +
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
}
}
OperationsServer::OperationsServer(OperationsState& state, int port, const std::string& address) : state_(state) {
    listener_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener_ < 0) throw std::runtime_error("Cannot create operations socket");
    try {
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET; endpoint.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1)
            throw std::invalid_argument("CHWELL_OPS_BIND requires numeric IPv4");
        int reuse = 1; setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        nonblocking(listener_);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0 || listen(listener_, 16) != 0)
            throw std::runtime_error("Cannot bind operations port");
        thread_ = std::thread([this] { run(); });
    } catch (...) { close(listener_); listener_ = -1; throw; }
}
OperationsServer::~OperationsServer() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
    if (listener_ >= 0) close(listener_);
}
void OperationsServer::run() {
    std::vector<Peer> peers;
    while (!stopping_) {
        std::vector<pollfd> fds{{listener_, POLLIN, 0}};
        for (const auto& peer : peers) fds.push_back({peer.fd, static_cast<short>(peer.output.empty() ? POLLIN : POLLOUT), 0});
        const int count = poll(fds.data(), fds.size(), 100);
        if (count < 0 && errno != EINTR) break;
        // Process only peers included in this poll, before accepting new peers.
        for (std::size_t i = peers.size(); i > 0; --i) {
            auto& peer = peers[i - 1];
            const auto events = fds[i].revents;
            bool done = Clock::now() >= peer.deadline || (events & (POLLERR | POLLHUP | POLLNVAL));
            if (!done && (events & POLLIN)) {
                char buffer[1024];
                const auto got = recv(peer.fd, buffer, sizeof(buffer), 0);
                if (got > 0) {
                    peer.input.append(buffer, static_cast<std::size_t>(got));
                    if (peer.input.size() > 4096) done = true;
                    else if (peer.input.find("\r\n\r\n") != std::string::npos) {
                        const auto line = peer.input.substr(0, peer.input.find("\r\n"));
                        if (line == "GET /live HTTP/1.1" || line == "GET /live HTTP/1.0" ||
                            line == "GET /ready HTTP/1.1" || line == "GET /ready HTTP/1.0") {
                            const bool readiness = line.find("/ready") != std::string::npos;
                            const auto body = state_.health(readiness);
                            // Derive code from the SAME state snapshot as the JSON.
                            const bool ok = nlohmann::json::parse(body).at("ok");
                            peer.output = response(ok ? 200 : 503, body);
                        } else if (line == "GET /metrics HTTP/1.1" || line == "GET /metrics HTTP/1.0")
                            peer.output = response(200, state_.metrics(), "text/plain; version=0.0.4; charset=utf-8");
                        else peer.output = response(400, "{\"ok\":false}\n");
                    }
                } else if (!got || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) done = true;
            }
            if (!done && !peer.output.empty() && (events & POLLOUT)) {
                const auto sent = send(peer.fd, peer.output.data() + peer.sent, peer.output.size() - peer.sent, MSG_NOSIGNAL);
                if (sent > 0) { peer.sent += static_cast<std::size_t>(sent); done = peer.sent == peer.output.size(); }
                else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) done = true;
            }
            if (done) { close(peer.fd); peers.erase(peers.begin() + static_cast<std::ptrdiff_t>(i - 1)); }
        }
        // A bounded batch also prevents connect floods starving shutdown/old peers.
        if (fds[0].revents & POLLIN) for (int i = 0; i < 16; ++i) {
            const int fd = accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) break;
            if (peers.size() >= 16) close(fd);
            else peers.push_back({fd, Clock::now() + std::chrono::seconds(2), {}, {}, 0});
        }
    }
    for (const auto& peer : peers) close(peer.fd);
}
int probe(int port, const std::string& kind) {
    if (kind != "live" && kind != "ready") return 2;
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 1;
    timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in endpoint{}; endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(static_cast<uint16_t>(port)); endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto request = "GET /" + kind + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    bool ok = false;
    if (connect(fd, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0 &&
        send(fd, request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size())) {
        std::string reply;
        char buffer[256];
        while (reply.size() < 4096 && reply.find("\r\n") == std::string::npos) {
            const auto got = recv(fd, buffer, sizeof(buffer), 0);
            if (got <= 0) break;
            reply.append(buffer, static_cast<std::size_t>(got));
        }
        ok = reply.rfind("HTTP/1.1 200 ", 0) == 0;
    }
    close(fd); return ok ? 0 : 1;
}
}
