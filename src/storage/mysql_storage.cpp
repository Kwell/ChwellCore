#include "chwell/storage/mysql_storage.h"
#include "chwell/core/logger.h"

#if defined(CHWELL_USE_MYSQL)
#include <mysql/mysql.h>
#include <cstring>
#endif

namespace chwell {
namespace storage {

MysqlStorage::MysqlStorage(const StorageConfig& config) : config_(config) {}

MysqlStorage::~MysqlStorage() {
    disconnect();
}

bool MysqlStorage::connect() {
#if defined(CHWELL_USE_MYSQL)
    MYSQL* mysql = mysql_init(nullptr);
    if (!mysql) {
        CHWELL_LOG_ERROR("MysqlStorage: mysql_init failed");
        return false;
    }

    const char* host = config_.host.empty() ? nullptr : config_.host.c_str();
    const char* user = config_.user.empty() ? nullptr : config_.user.c_str();
    const char* pass = config_.password.empty() ? nullptr : config_.password.c_str();
    const char* db = config_.database.empty() ? nullptr : config_.database.c_str();
    unsigned int port = config_.port > 0 ? static_cast<unsigned int>(config_.port) : 3306;

    if (!mysql_real_connect(mysql, host, user, pass, db, port, nullptr, 0)) {
        CHWELL_LOG_ERROR("MysqlStorage: connect failed: " +
                                       std::string(mysql_error(mysql)));
        mysql_close(mysql);
        return false;
    }

    const char* charset = "utf8mb4";
    auto it = config_.extra.find("charset");
    if (it != config_.extra.end()) charset = it->second.c_str();
    mysql_set_character_set(mysql, charset);

    conn_ = mysql;

    std::string table = "kv";
    it = config_.extra.find("table");
    if (it != config_.extra.end()) table = it->second;

    std::string create_sql =
        "CREATE TABLE IF NOT EXISTS `" + table + "` ("
        "`k` VARCHAR(512) PRIMARY KEY, "
        "`v` MEDIUMTEXT NOT NULL, "
        "`expire_at` BIGINT DEFAULT 0"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (mysql_query(mysql, create_sql.c_str()) != 0) {
        CHWELL_LOG_ERROR("MysqlStorage: create table failed: " +
                                       std::string(mysql_error(mysql)));
        disconnect();
        return false;
    }

    CHWELL_LOG_INFO("MysqlStorage: connected to " + config_.host + ":" +
                                  std::to_string(port) + "/" + config_.database);
    return true;
#else
    (void)config_;
    CHWELL_LOG_WARN(
        "MysqlStorage: not built with MySQL support, use -DCHWELL_USE_MYSQL=ON");
    return false;
#endif
}

void MysqlStorage::disconnect() {
#if defined(CHWELL_USE_MYSQL)
    if (conn_) {
        mysql_close(static_cast<MYSQL*>(conn_));
        conn_ = nullptr;
    }
#endif
}

StorageResult MysqlStorage::get(const std::string& key) {
#if defined(CHWELL_USE_MYSQL)
    if (!conn_) return StorageResult::failure("not connected");

    std::string table = "kv";
    auto it = config_.extra.find("table");
    if (it != config_.extra.end()) table = it->second;

    MYSQL* mysql = static_cast<MYSQL*>(conn_);
    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (!stmt) return StorageResult::failure(mysql_error(mysql));

    std::string sql = "SELECT v FROM `" + table +
                     "` WHERE k=? AND (expire_at=0 OR expire_at>UNIX_TIMESTAMP())";
    if (mysql_stmt_prepare(stmt, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        StorageResult r = StorageResult::failure(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return r;
    }

    MYSQL_BIND param;
    std::memset(&param, 0, sizeof(param));
    param.buffer_type = MYSQL_TYPE_STRING;
    param.buffer = const_cast<char*>(key.data());
    param.buffer_length = static_cast<unsigned long>(key.size());
    param.length = &param.buffer_length;
    mysql_stmt_bind_param(stmt, &param);

    if (mysql_stmt_execute(stmt) != 0) {
        StorageResult r = StorageResult::failure(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return r;
    }

    char value_buf[65536];
    unsigned long value_len = 0;
    MYSQL_BIND result;
    std::memset(&result, 0, sizeof(result));
    result.buffer_type = MYSQL_TYPE_STRING;
    result.buffer = value_buf;
    result.buffer_length = sizeof(value_buf);
    result.length = &value_len;
    mysql_stmt_bind_result(stmt, &result);

    StorageResult ret;
    if (mysql_stmt_fetch(stmt) == 0) {
        ret = StorageResult::success(std::string(value_buf, value_len));
    } else {
        ret = StorageResult::failure("key not found");
    }
    mysql_stmt_close(stmt);
    return ret;
#else
    (void)key;
    return StorageResult::failure("MysqlStorage not built with MySQL support");
#endif
}

StorageResult MysqlStorage::put(const std::string& key, const std::string& value,
                                  std::int64_t expire_at) {
#if defined(CHWELL_USE_MYSQL)
    if (!conn_) return StorageResult::failure("not connected");

    std::string table = "kv";
    auto it = config_.extra.find("table");
    if (it != config_.extra.end()) table = it->second;

    MYSQL* mysql = static_cast<MYSQL*>(conn_);
    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (!stmt) return StorageResult::failure(mysql_error(mysql));

    std::string sql = "REPLACE INTO `" + table + "` (k, v, expire_at) VALUES (?, ?, ?)";
    if (mysql_stmt_prepare(stmt, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        StorageResult r = StorageResult::failure(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return r;
    }

    MYSQL_BIND params[3];
    std::memset(params, 0, sizeof(params));

    unsigned long key_len = static_cast<unsigned long>(key.size());
    params[0].buffer_type = MYSQL_TYPE_STRING;
    params[0].buffer = const_cast<char*>(key.data());
    params[0].buffer_length = key_len;
    params[0].length = &key_len;

    unsigned long val_len = static_cast<unsigned long>(value.size());
    params[1].buffer_type = MYSQL_TYPE_STRING;
    params[1].buffer = const_cast<char*>(value.data());
    params[1].buffer_length = val_len;
    params[1].length = &val_len;

    params[2].buffer_type = MYSQL_TYPE_LONGLONG;
    params[2].buffer = &expire_at;
    mysql_stmt_bind_param(stmt, params);

    StorageResult ret;
    if (mysql_stmt_execute(stmt) == 0) {
        ret = StorageResult::success();
    } else {
        ret = StorageResult::failure(mysql_stmt_error(stmt));
    }
    mysql_stmt_close(stmt);
    return ret;
#else
    (void)key;
    (void)value;
    (void)expire_at;
    return StorageResult::failure("MysqlStorage not built with MySQL support");
#endif
}

StorageResult MysqlStorage::remove(const std::string& key) {
#if defined(CHWELL_USE_MYSQL)
    if (!conn_) return StorageResult::failure("not connected");

    std::string table = "kv";
    auto it = config_.extra.find("table");
    if (it != config_.extra.end()) table = it->second;

    MYSQL* mysql = static_cast<MYSQL*>(conn_);
    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (!stmt) return StorageResult::failure(mysql_error(mysql));

    std::string sql = "DELETE FROM `" + table + "` WHERE k=?";
    if (mysql_stmt_prepare(stmt, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        StorageResult r = StorageResult::failure(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return r;
    }

    MYSQL_BIND param;
    std::memset(&param, 0, sizeof(param));
    unsigned long key_len = static_cast<unsigned long>(key.size());
    param.buffer_type = MYSQL_TYPE_STRING;
    param.buffer = const_cast<char*>(key.data());
    param.buffer_length = key_len;
    param.length = &key_len;
    mysql_stmt_bind_param(stmt, &param);

    StorageResult ret;
    if (mysql_stmt_execute(stmt) == 0) {
        ret = StorageResult::success();
    } else {
        ret = StorageResult::failure(mysql_stmt_error(stmt));
    }
    mysql_stmt_close(stmt);
    return ret;
#else
    (void)key;
    return StorageResult::failure("MysqlStorage not built with MySQL support");
#endif
}

bool MysqlStorage::exists(const std::string& key) {
#if defined(CHWELL_USE_MYSQL)
    auto r = get(key);
    return r.ok;
#else
    (void)key;
    return false;
#endif
}

}  // namespace storage
}  // namespace chwell
