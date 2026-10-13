#include "chwell/cluster/mysql_session_store.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace chwell;
using namespace chwell::cluster;
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
        config.extra = {{"table", "request_contract_kv"}, {"connect_timeout", "2"},
            {"read_timeout", "5"}, {"write_timeout", "2"}};
        MysqlSessionStore a(config), b(config);
        if (argc == 2 && std::string(argv[1]) == "--expect-incompatible-journal") {
            require(!a.connect(), "Non-transactional journal must be rejected before business starts");
            std::cout << "PASS: incompatible request journal rejected at startup\n";
            return 0;
        }
        require(a.connect() && b.connect(), "Real MySQL connections required");
        const auto id = "request-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto key = "player:" + id;
        auto lease = a.acquire(id, "owner", "game", "inc", 30);
        require(lease.ok(), "Acquire request owner");
        require(a.apply(lease.lease, [&](auto& db) { return db.put(key, "0"); }).ok(), "Seed document");
        std::atomic<int> calls{0};
        const auto work = [&](auto& db, std::string& outcome) {
            ++calls;
            const auto old = db.get(key);
            require(old.ok, "Read counter");
            outcome = std::to_string(std::stoi(old.value) + 1);
            return db.put(key, outcome);
        };
        const std::string request = "increment-v1";
        DurableRequestResult receipt{"sentinel", true}, other;
        const bool lost = argc == 2 && std::string(argv[1]) == "--lost-commit";
        const bool before = argc == 2 && std::string(argv[1]) == "--lost-before-commit";
        const auto first = a.apply_once(lease.lease, "one", request, receipt, work);
        if (lost || before) {
            require(first.status == (lost ? SessionStatus::Unknown : SessionStatus::Unavailable), "Lost response classification");
            require(receipt.value == "sentinel" && receipt.replayed, "Unconfirmed result does not change caller output");
            const auto recovered = a.request_result(lease.lease, "one", request, receipt);
            require(lost ? recovered.ok() : recovered.status == SessionStatus::Missing, "Reconnected journal recovery");
            require(a.apply(lease.lease, [&](auto& db) {
                require(db.get(key).value == (lost ? "1" : "0"), "Document and journal commit/rollback together");
                return storage::StorageResult::success();
            }).ok(), "Read recovered state");
            require(a.apply_once(lease.lease, "one", request, receipt, work).ok(), "Explicit same-ID recovery");
            require(receipt.value == "1" && receipt.replayed == lost && calls == (lost ? 1 : 2), "Recovery cannot double-commit");
            require(a.request_result(lease.lease, "one", request, other).ok() && other.value == "1", "Committed result query");
            require(a.release(lease.lease).ok(), "Release fault owner");
            std::cout << (lost ? "PASS: lost COMMIT OK recovered from durable result without replay\n" :
                                "PASS: disconnect before COMMIT rolls back mutation and journal; same-ID retry executes once\n");
            return 0;
        }
        require(first.ok() && receipt.value == "1" && !receipt.replayed && calls == 1, "First result committed");
        require(b.apply_once(lease.lease, "one", request, other, work).ok() && other.value == "1" && other.replayed && calls == 1,
                "Second connection skips duplicate work");
        receipt = {"unchanged", false};
        require(b.apply_once(lease.lease, "one", "different-v1", receipt, work).status == SessionStatus::Conflict &&
                receipt.value == "unchanged" && calls == 1, "Same ID rejects changed meaning without work");
        require(b.request_result(lease.lease, "absent", request, receipt).status == SessionStatus::Missing &&
                receipt.value == "unchanged", "Missing query does not execute or replace output");
        DurableRequestResult left, right;
        std::promise<void> start;
        auto gate = start.get_future().share();
        auto worker = std::async(std::launch::async, [&] { gate.wait(); return b.apply_once(lease.lease, "race", request, right, work); });
        start.set_value();
        const auto winner = a.apply_once(lease.lease, "race", request, left, work);
        require(winner.ok() && worker.get().ok() && left.replayed != right.replayed &&
                left.value == "2" && right.value == "2" && calls == 2, "Concurrent publishers commit one mutation");
        std::promise<void> entered, release, reader_started;
        auto unlocked = release.get_future().share();
        DurableRequestResult staged, queried;
        auto executing = std::async(std::launch::async, [&] {
            return a.apply_once(lease.lease, "inflight", request, staged, [&](auto&, auto& value) {
                value = "committed later"; entered.set_value(); unlocked.wait(); return storage::StorageResult::success();
            });
        });
        entered.get_future().wait();
        auto reading = std::async(std::launch::async, [&] {
            reader_started.set_value(); return b.request_result(lease.lease, "inflight", request, queried);
        });
        reader_started.get_future().wait();
        const bool blocked = reading.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
        release.set_value();
        const auto committed = executing.get(), read = reading.get();
        require(blocked && committed.ok() && read.ok() && queried.value == staged.value,
                "Result query waits for the in-flight owner's transaction and sees its commit");
        // Missing rows for different players occupy the same index gap. Both
        // callbacks must enter before either inserts, without conflicting gap locks.
        const auto player_left = a.acquire("z" + id + "-a", "owner", "game", "inc", 30);
        const auto player_right = b.acquire("z" + id + "-b", "owner", "game", "inc", 30);
        require(player_left.ok() && player_right.ok(), "Acquire independent players");
        std::promise<void> entered_left, entered_right, insert;
        auto insert_gate = insert.get_future().share();
        auto arrived_left = entered_left.get_future(), arrived_right = entered_right.get_future();
        auto independent_left = std::async(std::launch::async, [&] {
            return a.apply_once(player_left.lease, "first", request, left, [&](auto&, auto& value) {
                value = "left"; entered_left.set_value(); insert_gate.wait(); return storage::StorageResult::success();
            });
        });
        auto independent_right = std::async(std::launch::async, [&] {
            return b.apply_once(player_right.lease, "first", request, right, [&](auto&, auto& value) {
                value = "right"; entered_right.set_value(); insert_gate.wait(); return storage::StorageResult::success();
            });
        });
        const auto timeout = std::chrono::seconds(1);
        const bool both_entered = arrived_left.wait_for(timeout) == std::future_status::ready &&
                                  arrived_right.wait_for(timeout) == std::future_status::ready;
        insert.set_value();
        const auto inserted_left = independent_left.get(), inserted_right = independent_right.get();
        require(both_entered && inserted_left.ok() && inserted_right.ok() &&
                left.value == "left" && right.value == "right" && !left.replayed && !right.replayed,
                "Independent players insert missing journal rows concurrently without gap-lock deadlock");
        require(a.release(player_left.lease).ok() && b.release(player_right.lease).ok(), "Release independent players");
        require(a.apply_once(lease.lease, "rejected", request, receipt, [&](auto& db, auto& value) {
            value = "not committed"; require(db.put(key, "999").ok, "Rejected write");
            return storage::StorageResult::failure("business rejected");
        }).status == SessionStatus::Invalid, "Rejected business rolls back");
        require(a.request_result(lease.lease, "rejected", request, receipt).status == SessionStatus::Missing, "Rejected work has no receipt");
        try {
            a.apply_once(lease.lease, "throw", request, receipt, [&](auto& db, auto&) -> storage::StorageResult {
                require(db.put(key, "888").ok, "Exceptional write"); throw std::runtime_error("business exception");
            });
            require(false, "Exception must propagate");
        } catch (const std::runtime_error& error) { require(std::string(error.what()) == "business exception", "Expected business exception"); }
        require(a.request_result(lease.lease, "throw", request, receipt).status == SessionStatus::Missing, "Exception has no receipt");
        require(a.apply_once(lease.lease, "oversize", request, receipt, [&](auto& db, auto& value) {
            value.assign(1024 * 1024 + 1, 'x'); return db.put(key, "777");
        }).status == SessionStatus::Invalid, "Oversized outcome rolls back business write");
        require(a.request_result(lease.lease, "oversize", request, receipt).status == SessionStatus::Missing, "Oversize has no receipt");
        require(a.apply_once(lease.lease, "", request, receipt, work).status == SessionStatus::Invalid &&
                a.apply_once(lease.lease, std::string(129, 'x'), request, receipt, work).status == SessionStatus::Invalid &&
                a.apply_once(lease.lease, "bad", std::string(8193, 'x'), receipt, work).status == SessionStatus::Invalid, "Input bounds");
        // Exact binary result/keys, including trailing spaces and zero bytes.
        const std::string binary("x\0y", 3);
        for (const auto& request_id : {std::string("Case"), std::string("case"), std::string("case "), binary}) {
            require(a.apply_once(lease.lease, request_id, binary, receipt, [&](auto&, auto& value) {
                value = binary; return storage::StorageResult::success();
            }).ok() && !receipt.replayed && receipt.value == binary, "Distinct binary request IDs");
            require(b.request_result(lease.lease, request_id, binary, receipt).ok() && receipt.value == binary, "Binary lookup");
        }
        auto other_config = config; other_config.extra["table"] = "request_other_kv";
        MysqlSessionStore namespace_store(other_config);
        require(namespace_store.connect() && namespace_store.request_result(lease.lease, "one", request, receipt).status == SessionStatus::Missing,
                "Document table namespaces the journal");
        require(a.release(lease.lease).ok(), "Release initial owner");
        const auto next = b.acquire(id, "next-owner", "new-game", "new-inc", 30);
        require(next.ok(), "Acquire replacement lease");
        require(a.request_result(lease.lease, "one", request, receipt).status == SessionStatus::Lost &&
                a.apply_once(lease.lease, "one", request, receipt, work).status == SessionStatus::Lost, "Stale owner cannot recover or replay");
        require(b.apply_once(next.lease, "one", request, receipt, work).ok() && receipt.replayed && receipt.value == "1" && calls == 2,
                "Original result survives owner/node/epoch changes and newer mutations");
        require(b.renew(next.lease, 1).ok(), "Short lease");
        require(b.apply_once(next.lease, "expired", request, receipt, [&](auto& db, auto& value) {
            require(db.put(key, "666").ok, "Expiring write"); value = "expired";
            std::this_thread::sleep_for(std::chrono::milliseconds(1400)); return storage::StorageResult::success();
        }).status == SessionStatus::Lost, "Expired work rolls back both records");
        const auto final = a.acquire(id, "final-owner", "game", "inc", 30);
        require(final.ok(), "Acquire after expiry");
        require(a.request_result(final.lease, "expired", request, receipt).status == SessionStatus::Missing, "Expired work has no receipt");
        require(a.apply(final.lease, [&](auto& db) {
            require(db.get(key).value == "2", "Rejected, exception, oversize, expiry and replay left counter intact");
            return db.remove(key);
        }).ok() && a.release(final.lease).ok(), "Final document and owner cleanup");
        std::cout << "PASS: durable results, concurrency, conflict, binary scopes, rollback, expiry and lease replacement\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
