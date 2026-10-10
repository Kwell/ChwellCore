#include "chwell/cluster/mysql_session_store.h"
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace chwell;
using namespace chwell::cluster;
using namespace std::string_literals;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string env(const char* key) { const auto* value = std::getenv(key); return value ? value : ""; }
}
int main(int argc, char** argv) {
    try {
        storage::StorageConfig config;
        config.host = env("CHWELL_DB_HOST"); config.port = std::stoi(env("CHWELL_DB_PORT"));
        config.database = env("CHWELL_DB_NAME"); config.user = env("CHWELL_DB_USER");
        config.password = env("CHWELL_DB_PASSWORD");
        config.extra = {{"table", env("CHWELL_SESSION_TEST_TABLE").empty() ? "session_contract_kv" : env("CHWELL_SESSION_TEST_TABLE")}, {"connect_timeout", "2"},
            {"read_timeout", "5"}, {"write_timeout", "2"}};
        MysqlSessionStore a(config), b(config);
        if (argc == 2 && std::string(argv[1]) == "--expect-incompatible") {
            require(!a.connect(), "Unsafe preexisting table must be rejected");
            std::cout << "PASS: incompatible legacy key table rejected\n";
            return 0;
        }
        require(a.connect() && b.connect(), "Two real connections required");
        const auto id = "contract-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto key = "player:" + id;
        auto lease = a.acquire(id, "owner-one", "game", "incarnation", 30);
        require(lease.ok(), "Acquire");
        require(b.acquire(id, "owner-two", "game", "incarnation", 30).status == SessionStatus::Busy, "Global conflict");
        require(a.apply(lease.lease, [&](auto& db) { return db.put(key, "0"); }).ok(), "Initial commit");
        require(!a.apply(lease.lease, [&](auto& db) {
            require(db.put(key, "999").ok, "Rollback write");
            return storage::StorageResult::failure("rejected");
        }).ok(), "Callback rejection");
        try {
            a.apply(lease.lease, [&](auto& db) -> storage::StorageResult {
                require(db.put(key, "888").ok, "Exception write");
                throw std::runtime_error("abort");
            });
            require(false, "Exception must propagate");
        } catch (const std::runtime_error& error) { require(std::string(error.what()) == "abort", "Expected callback exception"); }
        auto increment = [&](MysqlSessionStore& store) {
            for (int i = 0; i < 12; ++i) require(store.apply(lease.lease, [&](auto& db) {
                const auto value = db.get(key);
                require(value.ok, "Transactional read");
                return db.put(key, std::to_string(std::stoi(value.value) + 1));
            }).ok(), "Serialized increment");
        };
        auto worker = std::async(std::launch::async, [&] { increment(b); });
        increment(a); worker.get();
        require(a.apply(lease.lease, [&](auto& db) {
            require(db.get(key).value == "24", "No lost updates or leaked rollback");
            return storage::StorageResult::success();
        }).ok(), "Read result");
        require(a.release(lease.lease).ok(), "Release");
        auto replacement = b.acquire(id, "owner-two", "game", "incarnation-two", 30);
        require(replacement.ok() && std::stoull(replacement.lease.epoch) > std::stoull(lease.lease.epoch), "Monotonic epoch");
        bool called = false;
        require(a.apply(lease.lease, [&](auto&) { called = true; return storage::StorageResult::success(); }).status == SessionStatus::Lost && !called, "Stale callback fenced");
        require(a.renew(lease.lease, 30).status == SessionStatus::Lost, "Stale renew");
        require(a.release(lease.lease).status == SessionStatus::Lost, "Stale release");
        require(a.lookup(id).lease.owner == replacement.lease.owner, "Replacement survives stale owner");
        require(b.release(replacement.lease).ok(), "Release replacement");
        auto short_lease = a.acquire(id, "slow", "game", "incarnation", 1);
        require(short_lease.ok(), "Short acquire");
        std::promise<void> entered;
        auto waiting = entered.get_future();
        auto slow = std::async(std::launch::async, [&] {
            return a.apply(short_lease.lease, [&](auto& db) {
                entered.set_value();
                require(db.put(key, "777").ok, "Expiring write");
                std::this_thread::sleep_for(std::chrono::milliseconds(1400));
                return storage::StorageResult::success();
            });
        });
        waiting.get();
        // Starts before expiry, waits for the row lock, must use fresh DB time.
        require(b.renew(short_lease.lease, 30).status == SessionStatus::Lost, "Lock waiter must not resurrect expired lease");
        require(slow.get().status == SessionStatus::Lost, "Expiry during work rolls back");
        MysqlSessionStore reopened(config);
        auto after_expiry = reopened.acquire(id, "reconnected", "game", "incarnation", 30);
        require(after_expiry.ok() && std::stoull(after_expiry.lease.epoch) > std::stoull(short_lease.lease.epoch), "Epoch survives reconnect and expiry");
        require(reopened.apply(after_expiry.lease, [&](auto& db) {
            require(db.get(key).value == "24", "Expired transaction left document unchanged");
            return db.remove(key);
        }).ok(), "Expiry rollback verified");
        require(reopened.release(after_expiry.lease).ok(), "Cleanup document lease");
        const std::string binary_id = id + "'\\\0x"s;
        auto binary = a.acquire(binary_id, "owner'\\", "node", "inc", 30);
        require(binary.ok() && b.lookup(binary_id).lease.owner == "owner'\\", "Binary SQL literal safety");
        require(a.release(binary.lease).ok(), "Release binary lease");
        auto upper = a.acquire(id + "A", "case-one", "node", "inc", 30);
        auto lower = b.acquire(id + "a", "case-two", "node", "inc", 30);
        require(upper.ok() && lower.ok(), "Case-sensitive lease identities");
        const auto upper_key = key + "A", lower_key = key + "a";
        require(a.apply(upper.lease, [&](auto& db) { return db.put(upper_key, "upper"); }).ok(), "Upper document");
        require(b.apply(lower.lease, [&](auto& db) { return db.put(lower_key, "lower"); }).ok(), "Lower document");
        require(a.apply(upper.lease, [&](auto& db) {
            require(db.get(upper_key).value == "upper" && db.get(lower_key).value == "lower", "Case-sensitive documents");
            return db.remove(upper_key);
        }).ok(), "Case documents verified");
        require(b.apply(lower.lease, [&](auto& db) { return db.remove(lower_key); }).ok(), "Remove lower");
        require(a.release(upper.lease).ok() && b.release(lower.lease).ok(), "Release case leases");
        require(a.acquire("", "owner", "node", "inc", 1).status == SessionStatus::Invalid, "Invalid input");
        std::cout << "PASS: MySQL two-connection fencing, epochs, rollback, expiry and serialized writes\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
