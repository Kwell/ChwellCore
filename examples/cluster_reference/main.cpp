// Independent reference built from ChwellCore APIs. See README for scope.
#include "cluster_player.h"
#include "operations.h"
#include "chwell/service/app_host.h"
#include "chwell/discovery/consul_discovery.h"
#include "chwell/cluster/discovery_router.h"
#include "chwell/cluster/tcp_rpc_transport.h"
#include "chwell/protocol/parser.h"
#include "chwell/cluster/mysql_session_store.h"
#include "chwell/storage/orm/repository.h"
#include "chwell/sync/schema_sync.h"
#include "chwell/sync/sync_wire.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <future>
#include <iostream>
#include <limits>
#include <random>
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
volatile std::sig_atomic_t draining = 0;
void stop_signal(int) { draining = 1; stopping = 1; }
void drain_signal(int) { draining = 1; }
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
bool valid_request_id(const std::string& id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
int advance_steps(const Json& command) {
    const auto found = command.find("steps");
    if (found == command.end()) return 1;
    if (!found->is_number_integer() || *found < 1 || *found > 100) return 0;
    return found->get<int>();
}
std::string advance_command(int steps) {
    // Stable business semantics; exclude stream, sequence, credentials and lease epoch.
    return Json{{"action", "advance"}, {"version", 1}, {"steps", steps}}.dump();
}
storage::StorageConfig database_config() {
    storage::StorageConfig config;
    config.host = env("CHWELL_DB_HOST", "127.0.0.1");
    config.port = port_number(env("CHWELL_DB_PORT", "3306"));
    config.database = env("CHWELL_DB_NAME", "chwell");
    config.user = env("CHWELL_DB_USER", "chwell"); config.password = env("CHWELL_DB_PASSWORD");
    config.extra = {{"table", "cluster_kv"}, {"connect_timeout", "2"}, {"read_timeout", "2"}, {"write_timeout", "2"}};
    return config;
}
std::string process_identity() {
    std::random_device entropy;
    std::string id;
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        const auto value = entropy();
        for (int shift = 28; shift >= 0; shift -= 4) id += hex[(value >> shift) & 15];
    }
    return id;
}
Json session_failure(const cluster::SessionResult& result) {
    switch (result.status) {
    case cluster::SessionStatus::Busy: return failure("already_logged_in");
    case cluster::SessionStatus::Lost: return failure("session_lost");
    case cluster::SessionStatus::Unknown: return failure("outcome_unknown");
    case cluster::SessionStatus::Conflict: return failure("request_id_conflict");
    case cluster::SessionStatus::Missing: return failure("request_not_found");
    case cluster::SessionStatus::Invalid:
        return failure(result.error.empty() ? "bad_request" : result.error.c_str());
    default: return failure("storage_unavailable");
    }
}
std::uint64_t counter(const Json& command, const char* key) {
    const auto value = command.at(key).get<std::string>();
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size() || value.empty() ||
        std::to_string(parsed) != value) throw Json::type_error::create(302, "Invalid sync counter", &command);
    return parsed;
}
Json packet_json(const sync::SyncWirePacket& wire, bool owner) {
    std::string bytes;
    if (!sync::encode_sync_wire(wire, *Player::entity_schema(), owner, bytes, nullptr, 3072))
        return Json();
    const char* digits = "0123456789abcdef";
    std::string hex;
    for (unsigned char byte : bytes) { hex += digits[byte >> 4]; hex += digits[byte & 15]; }
    return {{"encoding", "chwell-sync-v1-hex"}, {"data", hex}};
}

// Network callbacks only copy into a bounded inbox. Discovery, sessions, schema
// and blocking DB/RPC belong to one dedicated worker, never an IO reactor.
class Inbox : public service::Component {
public:
    explicit Inbox(reference::OperationsState& operations) : operations_(operations) {}
    void begin_drain(int seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (draining_) return;
        accepting_ = false;
        drain_deadline_ = Clock::now() + std::chrono::seconds(seconds);
        draining_ = true;
        operations_.drain();
        wake_.notify_one();
    }
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
            operations_.finished();
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
            if (!accepting_) { operations_.rejected.inc(); return; }
            const auto id = conn->conn_id();
            if ((!admitted_.count(id) && admitted_.size() >= 128) ||
                queue_.size() >= 1024 || queued_bytes_ + bytes.size() > 1024 * 1024) {
                rejected = true;
            } else {
                admitted_.insert(id);
                queued_bytes_ += bytes.size();
                queue_.push_back({conn, std::string(bytes), false});
                operations_.queued.set(queue_.size());
            }
        }
        if (rejected) { operations_.rejected.inc(); conn->close(); }
        wake_.notify_one();
    }
    void on_disconnect(const net::TcpConnectionPtr& conn) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_ || !admitted_.count(conn->conn_id()) || !closing_.insert(conn->conn_id()).second) return;
            // At most one disconnect per admitted connection; reserve independent
            // of the data limit so overload cannot leak parser/session state.
            queue_.push_back({conn, {}, true});
            operations_.queued.set(queue_.size());
        }
        wake_.notify_one();
    }
protected:
    reference::OperationsState& operations_;
    bool drain_expired() const { return draining_ && Clock::now() >= drain_deadline_; }
    virtual bool startup() { return true; }
    virtual void shutdown() {}
    virtual void withdraw() {}
    virtual void tick() {}
    virtual Json request(std::uint64_t, const Json&) = 0;
    virtual void disconnected(std::uint64_t) {}
    virtual bool rpc_peer() const { return false; }
private:
    struct Input { net::TcpConnectionPtr conn; std::string bytes; bool closed; };
    struct Stream { protocol::Parser parser; net::TcpConnectionPtr conn; Clock::time_point active; };
    void stop() {
        begin_drain(15);
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    void run() {
        auto next_tick = Clock::now();
        bool withdrawn = false;
        while (true) {
            Input input;
            bool drain = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_until(lock, next_tick, [&] { return draining_ || !queue_.empty(); });
                drain = draining_;
                if (drain_expired()) {
                    operations_.dropped.inc(queue_.size()); queue_.clear(); queued_bytes_ = 0;
                    operations_.queued.set(0);
                }
                if (!queue_.empty()) {
                    input = std::move(queue_.front()); queue_.pop_front();
                    queued_bytes_ -= input.bytes.size();
                    operations_.queued.set(queue_.size());
                }
            }
            operations_.worker_progress();
            if (drain && !withdrawn) { withdraw(); withdrawn = true; }
            if (drain && !input.conn) break;
            if (!drain && Clock::now() >= next_tick) {
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
                // An already executing transaction is never interrupted or replayed.
                if (drain_expired()) { operations_.dropped.inc(); break; }
                const std::size_t prefix = rpc_peer() ? 4 : 0;
                if (message.cmd != 1 || message.body.size() < prefix || message.body.size() > 8192) {
                    input.conn->close(); break;
                }
                Json response;
                operations_.inflight.set(1);
                try {
                    auto command = Json::parse(message.body.begin() + prefix, message.body.end());
                    response = command.is_object() ? request(id, command) : failure("bad_request");
                } catch (const Json::exception&) { response = failure("bad_request"); }
                operations_.inflight.set(0);
                operations_.requests.inc();
                if (!response.value("ok", false)) operations_.errors.inc();
                operations_.worker_progress();
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
    bool accepting_ = false;
    std::atomic<bool> draining_{false};
    Clock::time_point drain_deadline_{}; // Written before draining_ is published.
    std::size_t queued_bytes_ = 0;
    std::deque<Input> queue_;
    std::unordered_set<std::uint64_t> admitted_;
    std::unordered_set<std::uint64_t> closing_;
    std::unordered_map<std::uint64_t, Stream> streams_;
    std::thread worker_;
};

class Game final : public Inbox {
public:
    Game(reference::OperationsState& operations, discovery::ConsulConfig config, std::string id, int port, std::string generation)
        : Inbox(operations), discovery_(discovery::make_consul_discovery(config)), key_(env("CHWELL_CLUSTER_TOKEN")),
          generation_(std::move(generation)) {
        instance_.service_id = "persistent-game"; instance_.instance_id = std::move(id);
        instance_.port = static_cast<std::uint16_t>(port); instance_.metadata["incarnation"] = process_identity();
    }
    ~Game() override { PreShut(); }
    std::string name() const override { return "PersistentGame"; }
protected:
    bool rpc_peer() const override { return true; }
    bool startup() override {
        if (key_.empty()) return false;
        instance_.host = advertised_ipv4(); // Resolve once; publish a numeric RPC endpoint.
        db_ = std::make_unique<cluster::MysqlSessionStore>(database_config());
        if (!db_->connect()) return false;
        registered_ = discovery_->register_service(instance_);
        heartbeat_ = Clock::now() + std::chrono::seconds(1);
        return registered_;
    }
    void shutdown() override {
        withdraw();
        db_.reset();
    }
    void withdraw() override {
        if (registered_) discovery_->deregister_service(instance_.instance_id);
        registered_ = false;
    }
    void tick() override {
        if (Clock::now() < heartbeat_) return;
        const bool database = db_->health_check();
        if (!database) withdraw();
        else if (!registered_ || !discovery_->heartbeat(instance_.instance_id))
            registered_ = discovery_->register_service(instance_);
        operations_.dependencies(database, registered_, registered_ ? 1 : 0);
        heartbeat_ = Clock::now() + std::chrono::seconds(1);
    }
    Json request(std::uint64_t, const Json& command) override {
        if (command.value("cluster_token", std::string()) != key_) return failure("unauthorized");
        if (!command.contains("sync_version") || !command.at("sync_version").is_number_integer() ||
            command.at("sync_version") != sync::sync_wire_version) return failure("unsupported_sync_version");
        const auto action = command.at("action").get<std::string>();
        const auto viewer = command.at("player").get<std::string>();
        const auto id = action == "observe" ? command.at("target").get<std::string>() : viewer;
        if (!valid_player(viewer) || !valid_player(id)) return failure("bad_player");
        if (action != "login" && action != "get" && action != "advance" && action != "observe" && action != "request_result")
            return failure("bad_action");
        sync::SchemaPacket snapshot, delta;
        const auto& fence = command.at("lease");
        cluster::SessionLease lease{viewer, fence.at("owner").get<std::string>(),
            fence.at("node").get<std::string>(), fence.at("incarnation").get<std::string>(),
            fence.at("epoch").get<std::string>()};
        if (lease.node != instance_.instance_id || lease.incarnation != instance_.metadata.at("incarnation"))
            return failure("session_lost");
        const bool journaled = action == "advance" || action == "request_result";
        const auto request_id = journaled ? command.value("request_id", std::string()) : std::string();
        const int steps = journaled ? advance_steps(command) : 1;
        if (journaled && (!valid_request_id(request_id) || !steps)) return failure("bad_request_id_or_steps");
        cluster::DurableRequestResult receipt;
        if (action == "request_result") {
            const auto found = db_->request_result(lease, request_id, advance_command(steps), receipt);
            if (!found.ok()) return session_failure(found);
            const auto durable = Json::parse(receipt.value, nullptr, false);
            if (durable.is_discarded()) return failure("invalid_request_record");
            return {{"ok", true}, {"request_id", request_id}, {"result", durable}};
        }
        sync::SyncWirePacket wire;
        wire.schema_name = Player::entity_schema()->name(); wire.schema_version = Player::schema_version;
        wire.stream = command.at("stream").get<std::string>(); wire.sequence = counter(command, "sequence");
        wire.base_sequence = action == "advance" ? counter(command, "base_sequence") : 0;
        auto initial = Player(); initial.set_id(id);
        wire.packet = {id, action != "advance", action == "advance" ? std::vector<schema::FieldValue>{} : initial.snapshot(viewer == id)};
        if (packet_json(wire, viewer == id).is_null()) return failure("bad_request");
        const auto work = [&](storage::StorageInterface& transaction, bool mutate, std::string* durable) {
            auto player = std::make_shared<Player>();
            const auto stored = transaction.get("cluster_players:" + id);
            if (stored.ok) {
                storage::orm::Document document;
                if (!document.from_string(stored.value) || !player->load_document(document) || player->id() != id)
                    return storage::StorageResult::failure("corrupt_record");
            } else if (stored.error_msg == "key not found") {
                if (action != "login") return storage::StorageResult::failure("not_found");
                if (!player->set_id(id)) return storage::StorageResult::failure("bad_player");
                storage::orm::Repository<Player> repo(&transaction, "cluster_players");
                if (!repo.save(*player).ok) return storage::StorageResult::failure("storage_unavailable");
                player->clear_dirty();
            } else return storage::StorageResult::failure("storage_unavailable");
            sync::SchemaSyncRoom room;
            room.add_entity(player, id);
            room.subscribe(id, viewer, [&](const auto& packet) {
                // Stage packets locally; they become visible only after COMMIT.
                (packet.snapshot ? snapshot : delta) = packet;
            });
            if (mutate) {
                if (!player->set_level(player->get_level() + steps) || !player->set_gold(player->get_gold() + 10 * steps))
                    return storage::StorageResult::failure("limit_reached");
                storage::orm::Repository<Player> repo(&transaction, "cluster_players");
                if (!repo.save(*player).ok) return storage::StorageResult::failure("storage_unavailable");
                room.flush();
                if (durable) *durable = Json{{"schema_version", Player::schema_version},
                    {"level", player->get_level()}, {"gold", player->get_gold()}}.dump();
            }
            return storage::StorageResult::success();
        };
        const auto guarded = action == "advance" ? db_->apply_once(lease, request_id, advance_command(steps), receipt,
            [&](auto& transaction, auto& durable) { return work(transaction, true, &durable); }) :
            db_->apply(lease, [&](auto& transaction) { return work(transaction, false, nullptr); });
        if (!guarded.ok()) return session_failure(guarded);
        if (action == "advance" && receipt.replayed) {
            // The durable outcome is historical. A new stream needs CURRENT
            // state, never an old delta/sequence rewrapped as a new response.
            const auto refreshed = db_->apply(lease, [&](auto& transaction) { return work(transaction, false, nullptr); });
            if (!refreshed.ok()) return failure("outcome_unknown");
            wire.base_sequence = 0;
        }
        wire.packet = action == "advance" && !receipt.replayed ? delta : snapshot;
        const auto encoded = packet_json(wire, viewer == id);
        if (encoded.is_null()) return failure("outcome_unknown"); // Commit succeeded, no mutation replay.
        Json response = {{"ok", true}, {"node", instance_.instance_id}, {"generation", generation_}, {"epoch", lease.epoch},
                {"packet", encoded}};
        if (action == "advance") {
            const auto durable = Json::parse(receipt.value, nullptr, false);
            if (durable.is_discarded()) return failure("invalid_request_record");
            response["request"] = {{"id", request_id}, {"replayed", receipt.replayed}, {"result", durable}};
        }
        return response;
    }
private:
    std::shared_ptr<discovery::ConsulServiceDiscovery> discovery_;
    discovery::ServiceInstance instance_;
    std::unique_ptr<cluster::MysqlSessionStore> db_;
    std::string key_, generation_;
    bool registered_ = false;
    Clock::time_point heartbeat_;
};

class Gateway final : public Inbox {
public:
    Gateway(reference::OperationsState& operations, discovery::ConsulConfig config)
        : Inbox(operations), discovery_(discovery::make_consul_discovery(config)), registry_(std::make_shared<cluster::NodeRegistry>()),
          router_(registry_), routes_(*discovery_, registry_, router_, sessions_, "persistent-game"),
          authority_(database_config()), token_(env("CHWELL_DEMO_TOKEN")), key_(env("CHWELL_CLUSTER_TOKEN")),
          identity_(process_identity()) {}
    ~Gateway() override { PreShut(); }
    std::string name() const override { return "PersistentGateway"; }
protected:
    bool startup() override {
        router_.set_failover_retries(0);
        router_.set_transport_factory([](const auto& node) { return std::make_shared<cluster::TcpRpcTransport>(node); });
        return !token_.empty() && !key_.empty() && authority_.connect();
    }
    void shutdown() override {
        while (!players_.empty() && !drain_expired()) {
            disconnected(players_.begin()->first); operations_.worker_progress();
        }
        // Remaining leases expire in the database; never extend the shutdown budget per player.
        players_.clear(); operations_.sessions.set(0);
    }
    void tick() override {
        available_ = routes_.refresh();
        if (Clock::now() >= database_probe_) {
            database_available_ = authority_.health_check();
            database_probe_ = Clock::now() + std::chrono::seconds(1);
        }
        operations_.dependencies(database_available_, available_, registry_->get_all_nodes().size());
        const bool renew = Clock::now() >= renewal_;
        for (auto it = players_.begin(); it != players_.end();) {
            if (sessions_.node_of(std::to_string(it->first)).empty() ||
                (renew && !authority_.renew(it->second, 8).ok())) {
                const auto id = it->first; ++it; disconnected(id);
            } else ++it;
        }
        if (renew) renewal_ = Clock::now() + std::chrono::seconds(2);
        operations_.sessions.set(players_.size());
    }
    void disconnected(std::uint64_t id) override {
        sessions_.unbind(std::to_string(id));
        sequences_.erase(id);
        const auto found = players_.find(id);
        if (found != players_.end()) { authority_.release(found->second); players_.erase(found); }
        operations_.sessions.set(players_.size());
    }
    Json request(std::uint64_t conn, const Json& command) override {
        if (!command.contains("sync_version") || !command.at("sync_version").is_number_integer() ||
            command.at("sync_version") != sync::sync_wire_version) return failure("unsupported_sync_version");
        const auto action = command.at("action").get<std::string>();
        if (action == "status") {
            Json nodes = Json::array();
            for (const auto& node : registry_->get_all_nodes()) nodes.push_back(node.node_id);
            return {{"ok", available_}, {"nodes", nodes}, {"sessions", sessions_.count()}};
        }
        if (action == "logout") { disconnected(conn); return {{"ok", true}}; }
        if (!available_) return failure("discovery_unavailable");
        const auto session = std::to_string(conn);
        Json forwarded = {{"action", action}, {"cluster_token", key_}, {"sync_version", sync::sync_wire_version}};
        cluster::NodeInfo node;
        cluster::SessionLease lease;
        const bool login = action == "login";
        if (login) {
            if (!command.contains("schema_version") || !command.at("schema_version").is_number_integer() ||
                command.at("schema_version") != Player::schema_version) return failure("unsupported_schema_version");
            if (command.value("token", std::string()) != token_) return failure("unauthorized");
            const auto player = command.at("player").get<std::string>();
            if (!valid_player(player)) return failure("bad_player");
            if (players_.count(conn)) return failure("already_logged_in");
            if (!registry_->select_node_by_hash(player, node, "persistent-game")) return failure("no_game");
            const auto acquired = authority_.acquire(player, identity_ + ":" + session,
                node.node_id, node.incarnation, 8);
            if (!acquired.ok()) return session_failure(acquired);
            lease = acquired.lease;
            forwarded["player"] = player;
        } else {
            const auto found = players_.find(conn);
            if (found == players_.end()) return failure("login_required");
            if (action != "get" && action != "advance" && action != "observe" && action != "request_result")
                return failure("bad_action");
            lease = found->second;
            forwarded["player"] = lease.player; // Never trust client player/viewer/fence fields.
            if (action == "observe") {
                const auto target = command.at("target").get<std::string>();
                if (!valid_player(target)) return failure("bad_player");
                forwarded["target"] = target;
            }
        }
        if (action == "advance" || action == "request_result") {
            const auto request_id = command.value("request_id", std::string());
            const auto steps = advance_steps(command);
            if (!valid_request_id(request_id) || !steps) return failure("bad_request_id_or_steps");
            forwarded["request_id"] = request_id; forwarded["steps"] = steps;
        }
        forwarded["lease"] = {{"owner", lease.owner}, {"node", lease.node},
            {"incarnation", lease.incarnation}, {"epoch", lease.epoch}};
        const auto entity = action == "observe" ? forwarded.at("target").get<std::string>() : lease.player;
        const auto& history = sequences_[conn];
        const auto found_sequence = history.find(entity);
        if (!login && found_sequence == history.end() && history.size() >= 128) return failure("sync_entity_limit");
        const auto previous = login || found_sequence == history.end() ? 0 : found_sequence->second;
        if (action != "request_result" && previous == std::numeric_limits<std::uint64_t>::max()) {
            if (login) authority_.release(lease);
            disconnected(conn); return failure("sync_sequence_exhausted");
        }
        forwarded["stream"] = identity_ + ":" + session + ":" + lease.epoch;
        forwarded["sequence"] = std::to_string(action == "request_result" ? 0 : previous + 1);
        forwarded["base_sequence"] = std::to_string(previous);
        const auto text = forwarded.dump();
        const std::vector<char> request(text.begin(), text.end());
        std::vector<char> response;
        const bool delivered = login ? router_.forward_to_node(node, 1, request, response, 5000)
            : routes_.forward_session(session, 1, request, response, 5000);
        if (!delivered) {
            if (login) authority_.release(lease);
            disconnected(conn); return failure("outcome_unknown");
        }
        auto result = Json::parse(response.begin(), response.end());
        if (result.value("ok", false) && action != "request_result") sequences_[conn][entity] = previous + 1;
        if (login && result.value("ok", false)) {
            sessions_.bind(session, node.node_id, "persistent-game");
            players_[conn] = lease;
            operations_.sessions.set(players_.size());
        } else if (login) authority_.release(lease);
        else if (!result.value("ok", false) &&
            (result.value("error", std::string()) == "session_lost" ||
             result.value("error", std::string()) == "storage_unavailable" ||
             result.value("error", std::string()) == "outcome_unknown")) disconnected(conn);
        return result;
    }
private:
    std::shared_ptr<discovery::ConsulServiceDiscovery> discovery_;
    std::shared_ptr<cluster::NodeRegistry> registry_;
    cluster::RpcRouter router_;
    cluster::SessionLocator sessions_;
    cluster::DiscoveryRouter routes_;
    std::unordered_map<std::uint64_t, cluster::SessionLease> players_;
    std::unordered_map<std::uint64_t, std::map<std::string, std::uint64_t>> sequences_;
    cluster::MysqlSessionStore authority_;
    std::string token_, key_, identity_;
    bool available_ = false;
    bool database_available_ = false;
    Clock::time_point database_probe_{};
    Clock::time_point renewal_{};
};
} // namespace

int main(int argc, char** argv) {
    core::Logger::instance().set_level(core::LogLevel::Error);
    if (argc == 4 && std::string(argv[1]) == "probe") {
        try { return reference::probe(port_number(argv[2]), argv[3]); }
        catch (const std::exception&) { return 2; }
    }
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
        const int drain_seconds = port_number(env("CHWELL_DRAIN_SECONDS", "15"));
        if (drain_seconds > 60) throw std::invalid_argument("Drain budget must be 1..60 seconds");
        std::signal(SIGINT, stop_signal); std::signal(SIGTERM, stop_signal); std::signal(SIGUSR1, drain_signal);
        reference::OperationsState operations;
        std::unique_ptr<reference::OperationsServer> probes;
        const auto ops_port = env("CHWELL_OPS_PORT");
        if (!ops_port.empty()) probes = std::make_unique<reference::OperationsServer>(operations,
            port_number(ops_port), env("CHWELL_OPS_BIND", "127.0.0.1"));
        service::AppHost host;
        Inbox* inbox = nullptr;
        host.register_component_factory("role", [&](const auto&) -> std::unique_ptr<service::Component> {
            std::unique_ptr<Inbox> component;
            if (role == "game") component = std::make_unique<Game>(operations, config, argv[3], port, argv[5]);
            else component = std::make_unique<Gateway>(operations, config);
            inbox = component.get();
            return component;
        });
        service::AppManifest manifest;
        manifest.listen_port = port; manifest.worker_threads = 1; manifest.use_epoll = true;
        core::ComponentConfig entry; entry.name = "role"; manifest.components.push_back(entry);
        if (!host.configure(manifest) || !host.start()) { std::cerr << host.last_error() << '\n'; return 1; }
        operations.started();
        std::cout << "READY " << role << std::endl;
        bool drain_started = false;
        Clock::time_point finished{};
        while (true) {
            operations.main_progress();
            if (draining && !drain_started) { inbox->begin_drain(drain_seconds); drain_started = true; }
            if (stopping && operations.complete()) {
                if (finished == Clock::time_point{}) finished = Clock::now();
                // Best-effort response flush window, not a client ACK or replay guarantee.
                if (Clock::now() - finished >= std::chrono::seconds(1)) break;
            }
            host.update(); std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        operations.stopped();
        host.stop();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
