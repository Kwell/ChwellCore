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

    // 读取表名并消毒（防止 SQL 注入），消毒后存储到 table_ 供后续方法复用
    std::string table = "kv";
    it = config_.extra.find("table");
    if (it != config_.extra.end()) table = it->second;
    table_ = sanitize_table_name(table);
    if (table_ != table) {
        CHWELL_LOG_WARN("MysqlStorage: table name sanitized from '" + table + "' to '" + table_ + "'");
    }

    std::string create_sql =
        "CREATE TABLE IF NOT EXISTS `" + table_ + "` ("
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
    // 加锁保护，确保没有其他线程正在使用连接
    std::lock_guard<std::shared_mutex> lock(conn_mutex_);
    if (conn_) {
        mysql_close(static_cast<MYSQL*>(conn_));
        conn_ = nullptr;
    }
#endif
}

StorageResult MysqlStorage::get(const std::string& key) {
#if defined(CHWELL_USE_MYSQL)
    if (!conn_) return StorageResult::failure("not connected");

    // 加锁保护 MySQL 连接（MYSQL* 非线程安全）
    std::lock_guard<std::shared_mutex> lock(conn_mutex_);

    MYSQL* mysql = static_cast<MYSQL*>(conn_);
    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (!stmt) return StorageResult::failure(mysql_error(mysql));

    std::string sql = "SELECT v FROM `" + table_ +
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

    // 先用空缓冲区绑定，获取实际长度后再二次读取，避免固定 64KB 截断大值
    unsigned long value_len = 0;
    my_bool is_null = 0;
    MYSQL_BIND result;
    std::memset(&result, 0, sizeof(result));
    result.buffer_type   = MYSQL_TYPE_STRING;
    result.buffer        = nullptr;
    result.buffer_length = 0;
    result.length        = &value_len;
    result.is_null       = &is_null;
    mysql_stmt_bind_result(stmt, &result);

    int fetch_rc = mysql_stmt_fetch(stmt);
    StorageResult ret;
    if (fetch_rc == 0 || fetch_rc == MYSQL_DATA_TRUNCATED) {
        if (is_null) {
            ret = StorageResult::success("");
        } else {
            std::string buf(value_len, '\0');
            result.buffer        = &buf[0];
            result.buffer_length = value_len;
            mysql_stmt_fetch_column(stmt, &result, 0, 0);
            ret = StorageResult::success(buf);
        }
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

    // 加锁保护 MySQL 连接（MYSQL* 非线程安全）
    std::lock_guard<std::shared_mutex> lock(conn_mutex_);

    MYSQL* mysql = static_cast<MYSQL*>(conn_);
    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (!stmt) return StorageResult::failure(mysql_error(mysql));

    std::string sql = "REPLACE INTO `" + table_ + "` (k, v, expire_at) VALUES (?, ?, ?)";
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

    // 加锁保护 MySQL 连接（MYSQL* 非线程安全）
    std::lock_guard<std::shared_mutex> lock(conn_mutex_);

    MYSQL* mysql = static_cast<MYSQL*>(conn_);
    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (!stmt) return StorageResult::failure(mysql_error(mysql));

    std::string sql = "DELETE FROM `" + table_ + "` WHERE k=?";
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
    if (!conn_) return false;

    // 加锁保护 MySQL 连接（MYSQL* 非线程安全）
    std::lock_guard<std::shared_mutex> lock(conn_mutex_);

    MYSQL* mysql = static_cast<MYSQL*>(conn_);
    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (!stmt) return false;

    std::string sql =
        "SELECT 1 FROM `" + table_ +
        "` WHERE k=? AND (expire_at=0 OR expire_at>UNIX_TIMESTAMP()) LIMIT 1";
    if (mysql_stmt_prepare(stmt, sql.c_str(),
                           static_cast<unsigned long>(sql.size())) != 0) {
        mysql_stmt_close(stmt);
        return false;
    }

    MYSQL_BIND param;
    std::memset(&param, 0, sizeof(param));
    unsigned long key_len = static_cast<unsigned long>(key.size());
    param.buffer_type     = MYSQL_TYPE_STRING;
    param.buffer          = const_cast<char*>(key.data());
    param.buffer_length   = key_len;
    param.length          = &key_len;
    mysql_stmt_bind_param(stmt, &param);

    std::int64_t       one      = 0;
    my_bool            res_null = 0;
    MYSQL_BIND         res_bind;
    std::memset(&res_bind, 0, sizeof(res_bind));
    res_bind.buffer_type = MYSQL_TYPE_LONGLONG;
    res_bind.buffer      = &one;
    res_bind.is_null     = &res_null;
    mysql_stmt_bind_result(stmt, &res_bind);

    if (mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return false;
    }

    int col_result = mysql_stmt_fetch(stmt);
    mysql_stmt_close(stmt);
    return col_result == 0;
#else
    (void)key;
    return false;
#endif
}

std::vector<std::string> MysqlStorage::keys(const std::string& prefix) {
#if defined(CHWELL_USE_MYSQL)
    if (!conn_) return {};

    // 加锁保护 MySQL 连接（MYSQL* 非线程安全）
    std::lock_guard<std::shared_mutex> lock(conn_mutex_);

    MYSQL* mysql = static_cast<MYSQL*>(conn_);
    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (!stmt) return {};

    std::string sql = "SELECT k FROM `" + table_ +
                      "` WHERE (expire_at=0 OR expire_at>UNIX_TIMESTAMP())";
    bool has_prefix = !prefix.empty();
    if (has_prefix) {
        sql += " AND k LIKE ?";
    }

    if (mysql_stmt_prepare(stmt, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        mysql_stmt_close(stmt);
        return {};
    }

    // 构造 LIKE 参数：prefix + '%'，并转义 MySQL LIKE 通配符
    std::string like_pat;
    if (has_prefix) {
        like_pat.reserve(prefix.size() + 4);
        for (unsigned char c : prefix) {
            if (c == '%' || c == '_' || c == '\\') like_pat += '\\';
            like_pat += static_cast<char>(c);
        }
        like_pat += '%';

        MYSQL_BIND param;
        std::memset(&param, 0, sizeof(param));
        unsigned long pat_len = static_cast<unsigned long>(like_pat.size());
        param.buffer_type   = MYSQL_TYPE_STRING;
        param.buffer        = const_cast<char*>(like_pat.data());
        param.buffer_length = pat_len;
        param.length        = &pat_len;
        mysql_stmt_bind_param(stmt, &param);
    }

    if (mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return {};
    }

    unsigned long key_len = 0;
    char key_buf[512];
    MYSQL_BIND res;
    std::memset(&res, 0, sizeof(res));
    res.buffer_type   = MYSQL_TYPE_STRING;
    res.buffer        = key_buf;
    res.buffer_length = sizeof(key_buf);
    res.length        = &key_len;
    mysql_stmt_bind_result(stmt, &res);

    std::vector<std::string> result;
    // 处理 MYSQL_DATA_TRUNCATED（返回值 100）：键长度超过 512 字节时
    // 使用 mysql_stmt_fetch_column 获取完整数据，避免长键被静默丢弃
    while (true) {
        int rc = mysql_stmt_fetch(stmt);
        if (rc == 1) break;  // 错误
        if (rc == MYSQL_NO_DATA) break;  // 无数据

        if (rc == MYSQL_DATA_TRUNCATED || key_len > sizeof(key_buf)) {
            // 键被截断，分配足够大的缓冲区重新获取
            std::vector<char> big_buf(key_len + 1);
            MYSQL_BIND col_bind;
            std::memset(&col_bind, 0, sizeof(col_bind));
            col_bind.buffer_type   = MYSQL_TYPE_STRING;
            col_bind.buffer        = big_buf.data();
            col_bind.buffer_length = big_buf.size();
            col_bind.length        = &key_len;
            mysql_stmt_fetch_column(stmt, &col_bind, 0, 0);
            result.emplace_back(big_buf.data(), key_len);
        } else {
            result.emplace_back(key_buf, key_len);
        }
    }
    mysql_stmt_close(stmt);
    return result;
#else
    (void)prefix;
    return {};
#endif
}

}  // namespace storage
}  // namespace chwell
