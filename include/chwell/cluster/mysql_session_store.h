#pragma once

#include "chwell/storage/mysql_storage.h"
#include <functional>

namespace chwell { namespace cluster {

// Persistent authority in the same MySQL database as the guarded documents.
// All methods are blocking; call from a worker. Epochs are decimal strings to
// avoid loss of uint64 precision in JSON/Lua clients. Rows must never be deleted.
struct SessionLease {
    std::string player, owner, node, incarnation, epoch;
};
enum class SessionStatus { Ok, Busy, Lost, Unavailable, Unknown, Invalid };
struct SessionResult {
    SessionStatus status = SessionStatus::Unavailable;
    SessionLease lease;
    std::string error;
    bool ok() const { return status == SessionStatus::Ok; }
};

class MysqlSessionStore {
public:
    explicit MysqlSessionStore(const storage::StorageConfig& config);
    bool connect();
    SessionResult acquire(const std::string& player, const std::string& owner,
                          const std::string& node, const std::string& incarnation, int ttl_seconds);
    SessionResult renew(const SessionLease& lease, int ttl_seconds);
    SessionResult release(const SessionLease& lease);
    SessionResult lookup(const std::string& player);
    // Locks and validates the lease row, runs work using the SAME transactional
    // connection, rechecks expiry, then commits. Failed work rolls back. Work may
    // only get/put/remove documents; no connection/transaction manipulation,
    // external IO, nested store calls, or client-visible side effects.
    // Unknown means a COMMIT response was lost; never automatically replay work.
    SessionResult apply(const SessionLease& lease,
        const std::function<storage::StorageResult(storage::StorageInterface&)>& work);
private:
    SessionResult transact(const std::function<SessionResult()>& work);
    SessionResult select(const std::string& player, bool lock);
    bool execute(const std::string& sql);
    SessionResult unavailable();
    storage::MysqlStorage storage_;
};

} }
