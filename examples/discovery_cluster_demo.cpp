// Independently implemented discovery/RPC reference, not a production game server.
#include "chwell/service/app_host.h"
#include "chwell/discovery/consul_discovery.h"
#include "chwell/cluster/discovery_router.h"
#include "chwell/cluster/tcp_rpc_transport.h"
#include "chwell/protocol/parser.h"
#include <nlohmann/json.hpp>
#include <csignal>
#include <iostream>
#include <thread>

using namespace chwell;
using Json = nlohmann::json;
namespace {
volatile std::sig_atomic_t stopping = 0;
void stop_signal(int) { stopping = 1; }

class GameEcho : public service::Component {
public:
    explicit GameEcho(std::string identity) : identity_(std::move(identity)) {}
    std::string name() const override { return "GameEcho"; }
    void on_message(const net::TcpConnectionPtr& conn, std::string_view bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& message : parsers_[conn.get()].feed(bytes)) {
            if (message.cmd != 1 || message.body.size() < 4) { conn->close(); return; }
            std::vector<char> body(message.body.begin(), message.body.begin() + 4);
            const auto prefix = identity_ + "|";
            // Respect the existing 16-bit body length.
            if (prefix.size() + message.body.size() > 65535) { conn->close(); return; }
            body.insert(body.end(), prefix.begin(), prefix.end());
            body.insert(body.end(), message.body.begin() + 4, message.body.end());
            conn->send(protocol::serialize(protocol::Message(message.cmd, body)));
        }
    }
    void on_disconnect(const net::TcpConnectionPtr& conn) override {
        std::lock_guard<std::mutex> lock(mutex_);
        parsers_.erase(conn.get());
    }
private:
    std::string identity_;
    std::mutex mutex_;
    std::unordered_map<const net::TcpConnection*, protocol::Parser> parsers_;
};

int game(const discovery::ConsulConfig& config, const std::string& id, int port, const std::string& generation) {
    auto discovery = discovery::make_consul_discovery(config);
    service::AppHost host;
    host.register_component_factory("echo", [id, generation](const auto&) {
        return std::make_unique<GameEcho>(id + "@" + generation);
    });
    service::AppManifest manifest;
    manifest.listen_port = port;
    manifest.worker_threads = 1;
    manifest.use_epoll = true;
    core::ComponentConfig echo;
    echo.name = "echo";
    manifest.components.push_back(echo);
    if (!host.configure(manifest) || !host.start()) { std::cerr << host.last_error() << '\n'; return 1; }
    discovery::ServiceInstance instance;
    instance.service_id = "game"; instance.instance_id = id;
    instance.host = "127.0.0.1"; instance.port = static_cast<std::uint16_t>(port);
    instance.metadata["incarnation"] = generation;
    if (!discovery->register_service(instance)) { host.stop(); std::cerr << discovery->last_error() << '\n'; return 1; }
    std::signal(SIGTERM, stop_signal);
    std::signal(SIGINT, stop_signal);
    auto heartbeat_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::cout << "READY " << id << std::endl;
    while (!stopping) {
        host.update();
        if (std::chrono::steady_clock::now() >= heartbeat_at) {
            if (!discovery->heartbeat(id)) discovery->register_service(instance);
            heartbeat_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    discovery->deregister_service(id);
    host.stop();
    return 0;
}

int probe(const discovery::ConsulConfig& config) {
    auto discovery = discovery::make_consul_discovery(config);
    auto registry = std::make_shared<cluster::NodeRegistry>();
    cluster::RpcRouter router(registry);
    router.set_failover_retries(0);
    router.set_transport_factory([](const auto& node) { return std::make_shared<cluster::TcpRpcTransport>(node); });
    cluster::SessionLocator sessions;
    cluster::DiscoveryRouter routes(*discovery, registry, router, sessions, "game");
    std::string line;
    while (std::getline(std::cin, line)) {
        Json result;
        try {
            const auto command = Json::parse(line);
            const bool refreshed = routes.refresh();
            result["ok"] = refreshed;
            result["error"] = routes.last_error();
            const auto action = command.at("action").get<std::string>();
            std::vector<char> response;
            if (action == "route" || action == "session") {
                const auto payload = command.value("payload", std::string("ping"));
                const std::vector<char> request(payload.begin(), payload.end());
                result["ok"] = refreshed && (action == "route"
                    ? router.forward("game", command.at("key").get<std::string>(), 1, request, response, 1000)
                    : routes.forward_session(command.at("session").get<std::string>(), 1, request, response, 1000));
                result["response"] = std::string(response.begin(), response.end());
            } else if (action == "bind") {
                cluster::NodeInfo node;
                const auto id = command.at("node").get<std::string>();
                result["ok"] = refreshed && registry->find_node(id, node) &&
                    sessions.bind(command.at("session").get<std::string>(), id, "game");
            } else if (action != "refresh") result["ok"] = false;
            result["nodes"] = Json::array();
            for (const auto& node : registry->get_all_nodes()) result["nodes"].push_back(node.node_id);
            result["sessions"] = sessions.count();
            result["connections"] = router.connection_count();
        } catch (const std::exception& error) {
            result = {{"ok", false}, {"error", error.what()}};
        }
        std::cout << "RESULT " << result.dump() << std::endl;
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    core::Logger::instance().set_level(core::LogLevel::Error);
    if (argc < 3) { std::cerr << "Usage: game ENDPOINT ID PORT GENERATION | probe ENDPOINT\n"; return 2; }
    discovery::ConsulConfig config;
    config.endpoint = argv[2];
    config.ttl_seconds = 4; // The demo heartbeats every second.
    if (const char* token = std::getenv("CONSUL_HTTP_TOKEN")) config.token = token;
    try {
        const std::string role = argv[1];
        if (role == "probe" && argc == 3) return probe(config);
        if (role == "game" && argc == 6) {
            std::size_t parsed = 0;
            const std::string text = argv[4];
            const auto port = std::stoi(text, &parsed);
            if (parsed != text.size() || port < 1 || port > 65535) return 2;
            return game(config, argv[3], port, argv[5]);
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 2;
}
