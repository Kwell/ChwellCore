#include "chwell/cluster/mysql_session_store.h"
#include "chwell/core/logger.h"
#if defined(CHWELL_USE_MYSQL)
#include <mysql/mysql.h>
#endif

namespace chwell { namespace cluster {
namespace {
// Hex literals are independent of SQL escaping modes (NO_BACKSLASH_ESCAPES).
std::string literal(const std::string& value) {
    const char* digits = "0123456789abcdef";
    std::string sql = "X'";
    for (unsigned char c : value) { sql += digits[c >> 4]; sql += digits[c & 15]; }
    return sql + "'";
}
bool valid(const SessionLease& lease) {
    return !lease.player.empty() && lease.player.size() <= 64 &&
        !lease.owner.empty() && lease.owner.size() <= 128 &&
        !lease.node.empty() && lease.node.size() <= 128 &&
        !lease.incarnation.empty() && lease.incarnation.size() <= 128;
}
bool matches(const SessionLease& a, const SessionLease& b) {
    return a.player == b.player && a.owner == b.owner && a.node == b.node &&
        a.incarnation == b.incarnation && a.epoch == b.epoch;
}
SessionResult result(SessionStatus status, SessionLease lease = {}, std::string error = {}) {
    return {status, std::move(lease), std::move(error)};
}
}

MysqlSessionStore::MysqlSessionStore(const storage::StorageConfig& config) : storage_(config) {}
bool MysqlSessionStore::execute(const std::string& sql) {
#if defined(CHWELL_USE_MYSQL)
    if (!storage_.conn_) return false;
    auto* connection = static_cast<MYSQL*>(storage_.conn_);
    if (mysql_query(connection, sql.c_str()) == 0) return true;
    CHWELL_LOG_ERROR("MysqlSessionStore: SQL error " + std::to_string(mysql_errno(connection)));
    return false;
#else
    (void)sql; return false;
#endif
}
SessionResult MysqlSessionStore::unavailable() {
    storage_.disconnect(); // Never automatically replay a failed transaction.
    return result(SessionStatus::Unavailable);
}
bool MysqlSessionStore::connect() {
    std::lock_guard<std::recursive_mutex> lock(storage_.conn_mutex_);
    if (!storage_.conn_ && !storage_.connect()) return false;
    const bool ok = execute("CREATE TABLE IF NOT EXISTS chwell_session_leases ("
        "player VARBINARY(64) PRIMARY KEY, owner VARBINARY(128) NOT NULL, "
        "node VARBINARY(128) NOT NULL, incarnation VARBINARY(128) NOT NULL, "
        "epoch BIGINT UNSIGNED NOT NULL, expires DATETIME(6) NOT NULL) ENGINE=InnoDB");
    bool compatible = ok;
#if defined(CHWELL_USE_MYSQL)
    if (compatible) {
        compatible = execute("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
            "AND table_name IN (" + literal(storage_.table_) + ",X'636877656c6c5f73657373696f6e5f6c6561736573') "
            "AND engine='InnoDB'");
        if (compatible) {
            auto* rows = mysql_store_result(static_cast<MYSQL*>(storage_.conn_));
            const auto row = rows ? mysql_fetch_row(rows) : nullptr;
            compatible = row && std::string(row[0]) == "2";
            if (rows) mysql_free_result(rows);
        }
        if (compatible) {
            compatible = execute("SELECT COUNT(*) FROM information_schema.columns WHERE table_schema=DATABASE() "
                "AND table_name=" + literal(storage_.table_) + " AND column_name='k' "
                "AND collation_name='utf8mb4_bin'");
            if (compatible) {
                auto* rows = mysql_store_result(static_cast<MYSQL*>(storage_.conn_));
                const auto row = rows ? mysql_fetch_row(rows) : nullptr;
                compatible = row && std::string(row[0]) == "1";
                if (rows) mysql_free_result(rows);
            }
        }
        // Bound server lock waits independently of client socket read timeouts.
        compatible = compatible && execute("SET SESSION innodb_lock_wait_timeout=2");
    }
#endif
    if (!compatible) {
        CHWELL_LOG_ERROR("MysqlSessionStore: requires InnoDB tables and utf8mb4_bin document keys; see SESSION_OWNERSHIP.md");
        storage_.disconnect();
    }
    return compatible;
}
SessionResult MysqlSessionStore::select(const std::string& player, bool lock) {
#if defined(CHWELL_USE_MYSQL)
    if (lock) {
        // NOW() is fixed at statement start, which can precede a lock wait.
        // Take the row lock first, then evaluate expiry with a fresh clock.
        if (!execute("SELECT player FROM chwell_session_leases WHERE player=" + literal(player) + " FOR UPDATE"))
            return result(SessionStatus::Unavailable);
        auto* rows = mysql_store_result(static_cast<MYSQL*>(storage_.conn_));
        if (!rows) return result(SessionStatus::Unavailable);
        mysql_free_result(rows);
    }
    if (!execute("SELECT owner,node,incarnation,CAST(epoch AS CHAR),expires > NOW(6) "
                 "FROM chwell_session_leases WHERE player=" + literal(player) + (lock ? " FOR UPDATE" : "")))
        return result(SessionStatus::Unavailable);
    MYSQL_RES* rows = mysql_store_result(static_cast<MYSQL*>(storage_.conn_));
    if (!rows) return result(SessionStatus::Unavailable);
    SessionResult selected = result(SessionStatus::Lost);
    if (auto row = mysql_fetch_row(rows)) {
    auto sizes = mysql_fetch_lengths(rows);
        if (!sizes || !row[0] || !row[1] || !row[2] || !row[3] || !row[4]) {
            mysql_free_result(rows);
            return result(SessionStatus::Unavailable);
        }
        SessionLease lease{player, std::string(row[0], sizes[0]), std::string(row[1], sizes[1]),
                           std::string(row[2], sizes[2]), std::string(row[3], sizes[3])};
        // Compute before moving lease: function argument evaluation order is unspecified.
        const auto status = row[4][0] == '1' && !lease.owner.empty() ? SessionStatus::Ok : SessionStatus::Lost;
        selected = result(status, std::move(lease));
    }
    if (mysql_errno(static_cast<MYSQL*>(storage_.conn_)) != 0) selected = result(SessionStatus::Unavailable);
    mysql_free_result(rows);
    return selected;
#else
    (void)player; (void)lock; return result(SessionStatus::Unavailable);
#endif
}
SessionResult MysqlSessionStore::transact(const std::function<SessionResult()>& work) {
    std::lock_guard<std::recursive_mutex> lock(storage_.conn_mutex_);
    if (!storage_.conn_ && !connect()) return result(SessionStatus::Unavailable);
    if (!execute("START TRANSACTION")) return unavailable();
    SessionResult completed;
    try { completed = work(); }
    catch (...) { if (!execute("ROLLBACK")) storage_.disconnect(); throw; }
    if (!completed.ok()) {
        if (!execute("ROLLBACK") || completed.status == SessionStatus::Unavailable) return unavailable();
        return completed;
    }
    if (!execute("COMMIT")) { storage_.disconnect(); return result(SessionStatus::Unknown); }
    return completed;
}
SessionResult MysqlSessionStore::acquire(const std::string& player, const std::string& owner,
    const std::string& node, const std::string& incarnation, int ttl) {
    SessionLease desired{player, owner, node, incarnation, {}};
    if (!valid(desired) || ttl < 1 || ttl > 300) return result(SessionStatus::Invalid);
    return transact([&] {
        // Duplicate-key UPDATE takes an exclusive lock directly, avoiding the
        // shared-lock upgrade deadlock of concurrent INSERT IGNORE + FOR UPDATE.
        if (!execute("INSERT INTO chwell_session_leases (player,owner,node,incarnation,epoch,expires) VALUES (" + literal(player) +
            ",X'',X'',X'',0,'1970-01-01 00:00:00') ON DUPLICATE KEY UPDATE player=player"))
            return result(SessionStatus::Unavailable);
        auto old = select(player, true);
        if (old.status == SessionStatus::Unavailable) return old;
        if (old.ok()) return result(SessionStatus::Busy);
        if (old.lease.epoch.empty() || old.lease.epoch == "18446744073709551615") return result(SessionStatus::Invalid);
        if (!execute("UPDATE chwell_session_leases SET owner=" + literal(owner) + ",node=" + literal(node) +
            ",incarnation=" + literal(incarnation) + ",epoch=epoch+1,expires=DATE_ADD(NOW(6), INTERVAL " +
            std::to_string(ttl) + " SECOND) WHERE player=" + literal(player))) return result(SessionStatus::Unavailable);
        return select(player, true);
    });
}
SessionResult MysqlSessionStore::renew(const SessionLease& lease, int ttl) {
    if (!valid(lease) || ttl < 1 || ttl > 300) return result(SessionStatus::Invalid);
    return transact([&] {
        const auto current = select(lease.player, true);
        if (current.status == SessionStatus::Unavailable) return current;
        if (!current.ok() || !matches(lease, current.lease)) return result(SessionStatus::Lost);
        if (!execute("UPDATE chwell_session_leases SET expires=DATE_ADD(NOW(6), INTERVAL " +
            std::to_string(ttl) + " SECOND) WHERE player=" + literal(lease.player) + " AND expires > NOW(6)"))
            return result(SessionStatus::Unavailable);
#if defined(CHWELL_USE_MYSQL)
        if (mysql_affected_rows(static_cast<MYSQL*>(storage_.conn_)) != 1) return result(SessionStatus::Lost);
#endif
        return result(SessionStatus::Ok, lease);
    });
}
SessionResult MysqlSessionStore::release(const SessionLease& lease) {
    if (!valid(lease)) return result(SessionStatus::Invalid);
    return transact([&] {
        const auto current = select(lease.player, true);
        if (current.status == SessionStatus::Unavailable) return current;
        if (!matches(lease, current.lease)) return result(SessionStatus::Lost);
        if (!execute("UPDATE chwell_session_leases SET owner=X'',expires='1970-01-01 00:00:00' WHERE player=" +
            literal(lease.player))) return result(SessionStatus::Unavailable);
        return result(SessionStatus::Ok, lease);
    });
}
SessionResult MysqlSessionStore::lookup(const std::string& player) {
    if (player.empty() || player.size() > 64) return result(SessionStatus::Invalid);
    std::lock_guard<std::recursive_mutex> lock(storage_.conn_mutex_);
    if (!storage_.conn_ && !connect()) return result(SessionStatus::Unavailable);
    auto found = select(player, false);
    return found.status == SessionStatus::Unavailable ? unavailable() : found;
}
SessionResult MysqlSessionStore::apply(const SessionLease& lease,
    const std::function<storage::StorageResult(storage::StorageInterface&)>& work) {
    if (!valid(lease) || !work) return result(SessionStatus::Invalid);
    return transact([&] {
        auto current = select(lease.player, true);
        if (current.status == SessionStatus::Unavailable) return current;
        if (!current.ok() || !matches(lease, current.lease)) return result(SessionStatus::Lost);
        const auto changed = work(storage_);
        if (!changed.ok) return result(SessionStatus::Invalid, {}, changed.error_msg);
        current = select(lease.player, true); // Fresh statement time; expired work must roll back.
        if (!current.ok()) return current;
        return result(SessionStatus::Ok, lease);
    });
}

} }
