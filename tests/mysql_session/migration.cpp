#include "chwell/cluster/mysql_session_store.h"
#include "chwell/schema/storage_migration.h"
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace chwell;
using namespace chwell::cluster;
using namespace chwell::schema;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string env(const char* key) { const auto* value = std::getenv(key); return value ? value : ""; }
std::shared_ptr<const EntitySchema> definition(unsigned version) {
    FieldDefinition id;
    id.id = 1; id.name = "id"; id.stored = true; id.visibility = Visibility::Public;
    FieldDefinition count;
    count.id = 2; count.name = version == 1 ? "score" : "points";
    count.type = FieldType::Int64; count.default_value = std::int64_t{0}; count.stored = true;
    count.int_min = 0;
    return std::make_shared<const EntitySchema>("Player", "players", 1,
        std::vector<FieldDefinition>{id, count}, std::vector<FieldId>{}, version);
}
}
int main(int argc, char** argv) {
    try {
        storage::StorageConfig config;
        config.host = env("CHWELL_DB_HOST"); config.port = std::stoi(env("CHWELL_DB_PORT"));
        config.database = env("CHWELL_DB_NAME"); config.user = env("CHWELL_DB_USER");
        config.password = env("CHWELL_DB_PASSWORD");
        config.extra = {{"table", "migration_contract_kv"}, {"connect_timeout", "2"},
            {"read_timeout", "5"}, {"write_timeout", "2"}};
        MysqlSessionStore a(config), b(config);
        require(a.connect() && b.connect(), "Real MySQL connections required");
        const auto id = "migration-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto key = "players:" + id;
        auto lease = a.acquire(id, "maintenance", "worker", "inc", 30);
        require(lease.ok(), "Acquire migration owner");
        storage::orm::Document original;
        original.set_string("id", id); original.set_string("score", "42");
        const auto ttl = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() + 3600;
        require(a.apply(lease.lease, [&](auto& db) { return db.put(key, original.to_string(), ttl); }).ok(), "Seed legacy data");
        storage::StorageDocument before;
        require(a.read_document(lease.lease, key, before).ok(), "Read exact bytes and TTL");
        require(before.value == original.to_string() && before.expire_at == ttl, "Read preserves full document");
        StorageMigration migration(definition(1), definition(2), [](const auto&, auto& dest, auto*) {
            dest.set_string("points", dest.get_string("score")); dest.erase("score"); return true;
        });
        MigrationChange change;
        require(migration.prepare(before, change), "Dry run");
        storage::StorageDocument current;
        require(b.read_document(lease.lease, key, current).ok() && current.value == before.value, "Dry run does not write");
        const bool expired_work = argc == 2 && std::string(argv[1]) == "--expired-work";
        if (expired_work) require(a.renew(lease.lease, 1).ok(), "Short lease before delayed update");
        const auto applied = a.compare_exchange(lease.lease, change.before, change.after.value);
        const bool lost_commit = argc == 2 && std::string(argv[1]) == "--lost-commit";
        if (expired_work) {
            require(applied.status == SessionStatus::Lost, "Lease expiry after UPDATE rolls back");
            auto recovered = b.acquire(id, "recovery", "worker", "next-inc", 30);
            require(recovered.ok(), "Acquire after delayed transaction rollback");
            require(b.read_document(recovered.lease, key, current).ok() && current.value == before.value && current.expire_at == ttl,
                    "Failed execution preserves original bytes, version and TTL");
            require(b.apply(recovered.lease, [&](auto& db) { return db.remove(key); }).ok(), "Expiry test cleanup");
            require(b.release(recovered.lease).ok(), "Release expiry test lease");
            std::cout << "PASS: lease expiry after real UPDATE rolls migration back without partial version or value\n";
            return 0;
        }
        if (!(lost_commit ? applied.status == SessionStatus::Unknown : applied.ok()))
            throw std::runtime_error("Unexpected migration status: " + std::to_string(static_cast<int>(applied.status)));
        require(b.read_document(lease.lease, key, current).ok() && current.value == change.after.value && current.expire_at == ttl, "Value, version and TTL committed together");
        if (lost_commit) {
            // Reconnect the failed connection and reconcile the receipt, without
            // calling prepare or replaying a transform a second time.
            require(a.read_document(lease.lease, key, current).ok() && current.value == change.after.value,
                    "Reconnect confirms committed target after lost response");
            require(a.compare_exchange(lease.lease, change.before, change.after.value).status == SessionStatus::Conflict,
                    "Lost-response retry cannot overwrite committed target");
            require(b.compare_exchange(lease.lease, change.after, change.before.value).ok(), "Confirmed conditional rollback");
            require(b.apply(lease.lease, [&](auto& db) { return db.remove(key); }).ok(), "Fault test cleanup");
            require(b.release(lease.lease).ok(), "Release fault test lease");
            std::cout << "PASS: lost COMMIT response is Unknown; reconnect reconciles committed migration without replay\n";
            return 0;
        }
        require(b.compare_exchange(lease.lease, change.before, change.after.value).status == SessionStatus::Conflict, "Retry never replays conversion");
        require(a.compare_exchange(lease.lease, change.after, change.before.value).ok(), "Conditional rollback");
        require(a.read_document(lease.lease, key, current).ok() && current.value == before.value && current.expire_at == ttl, "Rollback restores exact legacy bytes and TTL");
        // Two independent connections race on the same receipt. Exactly one wins.
        auto racing = std::async(std::launch::async, [&] { return b.compare_exchange(lease.lease, before, change.after.value); });
        const auto first = a.compare_exchange(lease.lease, before, change.after.value);
        const auto second = racing.get();
        require((first.ok() && second.status == SessionStatus::Conflict) ||
                (second.ok() && first.status == SessionStatus::Conflict), "Two-connection CAS has one winner");
        require(a.apply(lease.lease, [&](auto& db) {
            auto doc = storage::orm::Document{}; require(doc.from_string(change.after.value), "Read migrated document");
            doc.set_string("points", "43"); return db.put(key, doc.to_string(), ttl);
        }).ok(), "Business writes after migration");
        require(b.compare_exchange(lease.lease, change.after, before.value).status == SessionStatus::Conflict, "Rollback refuses changed business data");
        require(a.read_document(lease.lease, key, current).ok(), "Read business data");
        const auto business = current.value;
        require(a.apply(lease.lease, [&](auto& db) { return db.put(key, business, ttl + 1); }).ok(), "Change TTL only");
        require(b.compare_exchange(lease.lease, current, before.value).status == SessionStatus::Conflict, "TTL-only change conflicts");
        require(a.release(lease.lease).ok(), "Release");
        auto next = b.acquire(id, "next", "worker", "new-inc", 30);
        require(next.ok(), "New owner");
        require(a.compare_exchange(lease.lease, current, before.value).status == SessionStatus::Lost, "Stale migration fenced");
        storage::StorageDocument sentinel{"untouched", "unchanged"};
        require(a.read_document(lease.lease, key, sentinel).status == SessionStatus::Lost && sentinel.key == "untouched", "Failed read leaves out untouched");
        require(b.apply(next.lease, [&](auto& db) { return db.put(key, business, 1); }).ok(), "Expired record");
        require(b.read_document(next.lease, key, current).status == SessionStatus::Missing, "Expired record cannot migrate");
        require(b.compare_exchange(next.lease, {key, business, 1}, before.value).status == SessionStatus::Missing, "Expired CAS does not resurrect");
        require(b.apply(next.lease, [&](auto& db) { return db.remove(key); }).ok(), "Cleanup");
        require(b.compare_exchange(next.lease, before, change.after.value).status == SessionStatus::Missing, "Deleted document stays deleted");
        require(b.release(next.lease).ok(), "Release final lease");
        std::cout << "PASS: real MySQL migration, dry-run, TTL, concurrent CAS, safe rollback and fencing\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
