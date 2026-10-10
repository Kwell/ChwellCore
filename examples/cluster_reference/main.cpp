// Independent reference built from ChwellCore APIs. See README for scope.
#include "cluster_player.h"
#include "chwell/service/app_host.h"
#include "chwell/discovery/consul_discovery.h"
#include "chwell/cluster/discovery_router.h"
#include "chwell/cluster/tcp_rpc_transport.h"
#include "chwell/protocol/parser.h"
#include "chwell/storage/mysql_storage.h"
#include "chwell/storage/orm/repository.h"
#include "chwell/sync/schema_sync.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <future>
#include <iostream>
#include <thread>
#include <unordered_set>
#include <arpa/inet.h>
#include <netdb.h>

using namespace chwell;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Player = generated::ClusterPlayer;
namespace {
volatile std::sig_atomic_t stopping = 0;
void stop_signal(int) { stopping = 1; }
std::string env(const char* key, std::string fallback = {}) {
    const auto* value = std::getenv(key);
    return value ? value : fallback;
}
int port_number(const std::string& value) {
    std::size_t used = 0;
    const int port = std::stoi(value, &used);
    if (used != value.size() || port < 1 || port > 65535) throw std::invalid_argument("Invalid port");
    return port;
}
bool valid_player(const std::string& id) {
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
std::string advertised_ipv4() {
    const auto hostname = env("CHWELL_ADVERTISE_HOST", "127.0.0.1");
    addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname.c_str(), nullptr, &hints, &result) != 0)
        throw std::invalid_argument("Cannot resolve advertised IPv4 address");
    char address[INET_ADDRSTRLEN]{};
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
    const bool ok = inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address)) != nullptr;
    freeaddrinfo(result);
    if (!ok) throw std::invalid_argument("Invalid advertised IPv4 address");
    return address;
}
Json failure(const char* code) { return {{"ok", false}, {"error", code}}; }
Json packet_json(const sync::SchemaPacket& packet) {
    Json fields = Json::object();
    for (const auto& field : packet.fields)
        std::visit([&](const auto& value) { fields[std::to_string(field.id)] = value; }, field.value);
    return {{"entity", packet.entity_id}, {"snapshot", packet.snapshot}, {"fields", fields}};
}

// Network callbacks only copy into a bounded inbox. Discovery, sessions, schema
// and blocking DB/RPC belong to one dedicated worker, never an IO reactor.
class Inbox : public service::Component {
public:
    ~Inbox() override { stop(); }
    bool Init() override {
        std::promise<bool> initialized;
        auto result = initialized.get_future();
        worker_ = std::thread([this, ready = std::move(initialized)]() mutable {
            bool started = false;
            try { started = startup(); } catch (const std::exception&) {}
            ready.set_value(started);
            if (started) run();
            shutdown();
        });
        const bool ok = result.get();
        { std::lock_guard<std::mutex> lock(mutex_); accepting_ = ok; }
        return ok;
    }
    bool PreShut() override { stop(); return true; }
    bool Shut() override { return true; }
    void on_message(const net::TcpConnectionPtr& conn, std::string_view bytes) override {
        bool rejected = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_) return;
            const auto id = conn->conn_id();
            if ((!admitted_.count(id) && admitted_.size() >= 128) ||
                queue_.size() >= 1024 || queued_bytes_ + bytes.size() > 1024 * 1024) {
                rejected = true;
            } else {
                admitted_.insert(id);
                queued_bytes_ += bytes.size();
                queue_.push_back({conn, std::string(bytes), false});
            }
        }
        if (rejected) conn->close();
        wake_.notify_one();
    }
    void on_disconnect(const net::TcpConnectionPtr& conn) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_ || !admitted_.count(conn->conn_id()) || !closing_.insert(conn->conn_id()).second) return;
            // At most one disconnect per admitted connection; reserve independent
            // of the data limit so overload cannot leak parser/session state.
            queue_.push_back({conn, {}, true});
        }
        wake_.notify_one();
    }
protected:
    virtual bool startup() { return true; }
    virtual void shutdown() {}
    virtual void tick() {}
    virtual Json request(std::uint64_t, const Json&) = 0;
    virtual void disconnected(std::uint64_t) {}
    virtual bool rpc_peer() const { return false; }
private:
    struct Input { net::TcpConnectionPtr conn; std::string bytes; bool closed; };
    struct Stream { protocol::Parser parser; net::TcpConnectionPtr conn; Clock::time_point active; };
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex_); accepting_ = false; stopping_ = true; queue_.clear(); }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    void run() {
        auto next_tick = Clock::now();
        while (true) {
            Input input;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_until(lock, next_tick, [&] { return stopping_ || !queue_.empty(); });
                if (stopping_) break;
                if (!queue_.empty()) {
                    input = std::move(queue_.front()); queue_.pop_front();
                    queued_bytes_ -= input.bytes.size();
                }
            }
            if (Clock::now() >= next_tick) {
                tick();
                for (auto it = streams_.begin(); it != streams_.end();) {
                    if (Clock::now() - it->second.active > std::chrono::seconds(30)) {
                        it->second.conn->close(); disconnected(it->first); it = streams_.erase(it);
                    } else ++it;
                }
                next_tick = Clock::now() + std::chrono::milliseconds(250);
            }
            if (!input.conn) continue;
            const auto id = input.conn->conn_id();
            if (input.closed) {
                streams_.erase(id); disconnected(id);
                std::lock_guard<std::mutex> lock(mutex_);
                admitted_.erase(id); closing_.erase(id);
                continue;
            }
            auto& stream = streams_[id];
            stream.conn = input.conn; stream.active = Clock::now();
            for (const auto& message : stream.parser.feed(std::string_view(input.bytes))) {
                const std::size_t prefix = rpc_peer() ? 4 : 0;
                if (message.cmd != 1 || message.body.size() < prefix || message.body.size() > 8192) {
                    input.conn->close(); break;
                }
                Json response;
                try {
                    auto command = Json::parse(message.body.begin() + prefix, message.body.end());
                    response = command.is_object() ? request(id, command) : failure("bad_request");
                } catch (const Json::exception&) { response = failure("bad_request"); }
                const auto text = response.dump();
                std::vector<char> body(message.body.begin(), message.body.begin() + prefix);
                body.insert(body.end(), text.begin(), text.end());
                input.conn->send(protocol::serialize(protocol::Message(1, body)));
            }
        }
        streams_.clear();
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    bool accepting_ = false, stopping_ = false;
    std::size_t queued_bytes_ = 0;
    std::deque<Input> queue_;
    std::unordered_set<std::uint64_t> admitted_;
    std::unordered_set<std::uint64_t> closing_;
    std::unordered_map<std::uint64_t, Stream> streams_;
    std::thread worker_;
};

class Game final : public Inbox {
public:
    Game(discovery::ConsulConfig config, std::string id, int port, std::string generation)
        : discovery_(discovery::make_consul_discovery(config)), key_(env("CHWELL_CLUSTER_TOKEN")) {
        instance_.service_id = "persistent-game"; instance_.instance_id = std::move(id);
        instance_.port = static_cast<std::uint16_t>(port); instance_.metadata["incarnation"] = std::move(generation);
    }
    ~Game() override { PreShut(); }
    std::string name() const override { return "PersistentGame"; }
protected:
    bool rpc_peer() const override { return true; }
    bool startup() override {
        if (key_.empty()) return false;
        instance_.host = advertised_ipv4(); // Resolve once; publish a numeric RPC endpoint.
        storage::StorageConfig config;
        config.host = env("CHWELL_DB_HOST", "127.0.0.1");
        config.port = port_number(env("CHWELL_DB_PORT", "3306"));
        config.database = env("CHWELL_DB_NAME", "chwell");
        config.user = env("CHWELL_DB_USER", "chwell"); config.password = env("CHWELL_DB_PASSWORD");
        config.extra = {{"table", "cluster_kv"}, {"connect_timeout", "2"}, {"read_timeout", "2"}, {"write_timeout", "2"}};
        db_ = std::make_unique<storage::MysqlStorage>(config);
        if (!db_->connect()) return false;
        registered_ = discovery_->register_service(instance_);
        heartbeat_ = Clock::now() + std::chrono::seconds(1);
        return registered_;
    }
    void shutdown() override {
        if (registered_) discovery_->deregister_service(instance_.instance_id);
        db_.reset();
    }
    void tick() override {
        if (Clock::now() < heartbeat_) return;
        if (!discovery_->heartbeat(instance_.instance_id)) discovery_->register_service(instance_);
        heartbeat_ = Clock::now() + std::chrono::seconds(1);
    }
    Json request(std::uint64_t, const Json& command) override {
        if (command.value("cluster_token", std::string()) != key_) return failure("unauthorized");
        const auto action = command.at("action").get<std::string>();
        const auto viewer = command.at("player").get<std::string>();
        const auto id = action == "observe" ? command.at("target").get<std::string>() : viewer;
        if (!valid_player(viewer) || !valid_player(id)) return failure("bad_player");
        if (action != "login" && action != "get" && action != "advance" && action != "observe") return failure("bad_action");
        auto player = std::make_shared<Player>();
        const auto stored = db_->get("cluster_players:" + id);
        if (stored.ok) {
            storage::orm::Document document;
            if (!document.from_string(stored.value) || !player->load_document(document) || player->id() != id)
                return failure("corrupt_record");
        } else if (stored.error_msg == "key not found") {
            if (action != "login") return failure("not_found");
            if (!player->set_id(id)) return failure("bad_player");
            storage::orm::Repository<Player> repo(db_.get(), "cluster_players");
            if (!repo.save(*player).ok) return failure("outcome_unknown");
            player->clear_dirty();
        } else {
            // Never reinterpret a storage failure as absence or retry a mutation.
            db_->disconnect(); db_->connect();
            return failure("storage_unavailable");
        }
        sync::SchemaSyncRoom room;
        Json snapshot, delta;
        room.add_entity(player, id);
        room.subscribe(id, viewer, [&](const auto& packet) {
            (packet.snapshot ? snapshot : delta) = packet_json(packet);
        });
        if (action == "advance") {
            if (!player->set_level(player->get_level() + 1) || !player->set_gold(player->get_gold() + 10))
                return failure("limit_reached");
            storage::orm::Repository<Player> repo(db_.get(), "cluster_players");
            if (!repo.save(*player).ok) { db_->disconnect(); db_->connect(); return failure("outcome_unknown"); }
            room.flush(); // Only committed fields reach the client.
        }
        return {{"ok", true}, {"node", instance_.instance_id}, {"generation", instance_.metadata.at("incarnation")},
                {"packet", action == "advance" ? delta : snapshot}};
    }
private:
    std::shared_ptr<discovery::ConsulServiceDiscovery> discovery_;
    discovery::ServiceInstance instance_;
    std::unique_ptr<storage::MysqlStorage> db_;
    std::string key_;
    bool registered_ = false;
    Clock::time_point heartbeat_;
};

class Gateway final : public Inbox {
public:
    explicit Gateway(discovery::ConsulConfig config)
        : discovery_(discovery::make_consul_discovery(config)), registry_(std::make_shared<cluster::NodeRegistry>()),
          router_(registry_), routes_(*discovery_, registry_, router_, sessions_, "persistent-game"),
          token_(env("CHWELL_DEMO_TOKEN")), key_(env("CHWELL_CLUSTER_TOKEN")) {}
    ~Gateway() override { PreShut(); }
    std::string name() const override { return "PersistentGateway"; }
protected:
    bool startup() override {
        router_.set_failover_retries(0);
        router_.set_transport_factory([](const auto& node) { return std::make_shared<cluster::TcpRpcTransport>(node); });
        return !token_.empty() && !key_.empty();
    }
    void tick() override {
        available_ = routes_.refresh();
        for (auto it = players_.begin(); it != players_.end();) {
            if (sessions_.node_of(std::to_string(it->first)).empty()) it = players_.erase(it);
            else ++it;
        }
    }
    void disconnected(std::uint64_t id) override { sessions_.unbind(std::to_string(id)); players_.erase(id); }
    Json request(std::uint64_t conn, const Json& command) override {
        const auto action = command.at("action").get<std::string>();
        if (action == "status") {
            Json nodes = Json::array();
            for (const auto& node : registry_->get_all_nodes()) nodes.push_back(node.node_id);
            return {{"ok", available_}, {"nodes", nodes}, {"sessions", sessions_.count()}};
        }
        if (action == "logout") { disconnected(conn); return {{"ok", true}}; }
        if (!available_) return failure("discovery_unavailable");
        const auto session = std::to_string(conn);
        Json forwarded = {{"action", action}, {"cluster_token", key_}};
        cluster::NodeInfo node;
        const bool login = action == "login";
        if (login) {
            if (command.value("token", std::string()) != token_) return failure("unauthorized");
            const auto player = command.at("player").get<std::string>();
            if (!valid_player(player)) return failure("bad_player");
            if (players_.count(conn) || std::any_of(players_.begin(), players_.end(), [&](const auto& entry) {
                    return entry.second == player;
                })) return failure("already_logged_in");
            if (!registry_->select_node_by_hash(player, node, "persistent-game")) return failure("no_game");
            forwarded["player"] = player;
        } else {
            const auto found = players_.find(conn);
            if (found == players_.end()) return failure("login_required");
            if (action != "get" && action != "advance" && action != "observe") return failure("bad_action");
            forwarded["player"] = found->second; // Never trust client player/viewer fields.
            if (action == "observe") {
                const auto target = command.at("target").get<std::string>();
                if (!valid_player(target)) return failure("bad_player");
                forwarded["target"] = target;
            }
        }
        const auto text = forwarded.dump();
        const std::vector<char> request(text.begin(), text.end());
        std::vector<char> response;
        const bool delivered = login ? router_.forward_to_node(node, 1, request, response, 5000)
            : routes_.forward_session(session, 1, request, response, 5000);
        if (!delivered) { disconnected(conn); return failure("outcome_unknown"); }
        auto result = Json::parse(response.begin(), response.end());
        if (login && result.value("ok", false)) {
            sessions_.bind(session, node.node_id, "persistent-game");
            players_[conn] = forwarded.at("player").get<std::string>();
        }
        return result;
    }
private:
    std::shared_ptr<discovery::ConsulServiceDiscovery> discovery_;
    std::shared_ptr<cluster::NodeRegistry> registry_;
    cluster::RpcRouter router_;
    cluster::SessionLocator sessions_;
    cluster::DiscoveryRouter routes_;
    std::unordered_map<std::uint64_t, std::string> players_;
    std::string token_, key_;
    bool available_ = false;
};
} // namespace

int main(int argc, char** argv) {
    core::Logger::instance().set_level(core::LogLevel::Error);
    if (argc != 4 && argc != 6) {
        std::cerr << "Usage: cluster_reference gateway CONSUL PORT | game CONSUL ID PORT GENERATION\n"; return 2;
    }
    try {
        const std::string role = argv[1];
        if (!((role == "gateway" && argc == 4) || (role == "game" && argc == 6))) return 2;
        const auto port = port_number(argv[role == "game" ? 4 : 3]);
        discovery::ConsulConfig config;
        config.endpoint = argv[2]; config.ttl_seconds = 4;
        config.token = env("CONSUL_HTTP_TOKEN"); config.request_timeout_ms = 1000;
        service::AppHost host;
        host.register_component_factory("role", [&](const auto&) -> std::unique_ptr<service::Component> {
            if (role == "game") return std::make_unique<Game>(config, argv[3], port, argv[5]);
            return std::make_unique<Gateway>(config);
        });
        service::AppManifest manifest;
        manifest.listen_port = port; manifest.worker_threads = 1; manifest.use_epoll = true;
        core::ComponentConfig entry; entry.name = "role"; manifest.components.push_back(entry);
        if (!host.configure(manifest) || !host.start()) { std::cerr << host.last_error() << '\n'; return 1; }
        std::signal(SIGINT, stop_signal); std::signal(SIGTERM, stop_signal);
        std::cout << "READY " << role << std::endl;
        while (!stopping) { host.update(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        host.stop();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
