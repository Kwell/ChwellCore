#include "chwell/storage/mongodb_storage.h"
#include "chwell/core/logger.h"

#if defined(CHWELL_USE_MONGODB)
#include <mongoc/mongoc.h>
#include <bson/bson.h>
#include <chrono>
#include <cstring>
#include <mutex>
#include <atomic>
#include <cstdlib>
#endif

namespace chwell {
namespace storage {

#if defined(CHWELL_USE_MONGODB)
// mongoc_init/cleanup 必须在整个进程生命周期内各调用一次。
// 用引用计数保证：第一个实例 connect 时 init，所有实例 disconnect 后 cleanup（通过 atexit）。
namespace {
std::mutex  g_mongoc_mutex;
std::atomic<int> g_mongoc_refcount{0};

void ensure_mongoc_init() {
    std::lock_guard<std::mutex> lk(g_mongoc_mutex);
    if (g_mongoc_refcount.fetch_add(1) == 0) {
        mongoc_init();
        // 进程退出时执行一次 cleanup
        std::atexit([]() { mongoc_cleanup(); });
    }
}

void release_mongoc_ref() {
    // 引用计数递减；cleanup 由 atexit 负责，此处不再主动调用
    g_mongoc_refcount.fetch_sub(1);
}
}  // namespace
#endif

MongodbStorage::MongodbStorage(const StorageConfig& config) : config_(config) {}

MongodbStorage::~MongodbStorage() {
    disconnect();
}

bool MongodbStorage::connect() {
#if defined(CHWELL_USE_MONGODB)
    ensure_mongoc_init();

    std::string uri_str = "mongodb://127.0.0.1:27017";
    auto it = config_.extra.find("uri");
    if (it != config_.extra.end()) {
        uri_str = it->second;
    } else if (!config_.host.empty()) {
        uri_str = "mongodb://" + config_.host + ":" +
                  std::to_string(config_.port > 0 ? config_.port : 27017);
    }

    bson_error_t error;
    mongoc_client_t* client = mongoc_client_new_with_error(uri_str.c_str(), &error);
    if (!client) {
        CHWELL_LOG_ERROR("MongodbStorage: connect failed: " +
                                       std::string(error.message));
        release_mongoc_ref();
        return false;
    }

    std::string db_name = config_.database.empty() ? "chwell" : config_.database;
    std::string coll_name = "kv";
    it = config_.extra.find("collection");
    if (it != config_.extra.end()) coll_name = it->second;

    mongoc_collection_t* coll =
        mongoc_client_get_collection(client, db_name.c_str(), coll_name.c_str());

    client_ = client;
    collection_ = coll;

    CHWELL_LOG_INFO("MongodbStorage: connected to " + uri_str +
                                  "/" + db_name + "." + coll_name);
    return true;
#else
    (void)config_;
    CHWELL_LOG_WARN(
        "MongodbStorage: not built with MongoDB support, use -DCHWELL_USE_MONGODB=ON");
    return false;
#endif
}

void MongodbStorage::disconnect() {
#if defined(CHWELL_USE_MONGODB)
    if (collection_) {
        mongoc_collection_destroy(static_cast<mongoc_collection_t*>(collection_));
        collection_ = nullptr;
    }
    if (client_) {
        mongoc_client_destroy(static_cast<mongoc_client_t*>(client_));
        client_ = nullptr;
        release_mongoc_ref();
    }
#endif
}

StorageResult MongodbStorage::get(const std::string& key) {
#if defined(CHWELL_USE_MONGODB)
    if (!collection_) return StorageResult::failure("not connected");

    mongoc_collection_t* coll = static_cast<mongoc_collection_t*>(collection_);
    bson_t* query = bson_new();
    BSON_APPEND_UTF8(query, "_id", key.c_str());

    bson_error_t error;
    bson_t* doc = mongoc_collection_find_one_with_opts(coll, query, nullptr, nullptr, &error);
    bson_destroy(query);

    if (!doc) {
        if (error.code != 0) {
            return StorageResult::failure(error.message);
        }
        return StorageResult::failure("key not found");
    }

    bson_iter_t iter;
    std::string value;
    std::int64_t expire_at = 0;
    if (bson_iter_init(&iter, doc)) {
        while (bson_iter_next(&iter)) {
            const char* key_name = bson_iter_key(&iter);
            if (std::strcmp(key_name, "v") == 0 && BSON_ITER_HOLDS_UTF8(&iter)) {
                uint32_t len;
                const char* str = bson_iter_utf8(&iter, &len);
                value.assign(str, len);
            } else if (std::strcmp(key_name, "expire_at") == 0 && BSON_ITER_HOLDS_INT64(&iter)) {
                expire_at = bson_iter_int64(&iter);
            }
        }
    }

    if (expire_at > 0) {
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        if (expire_at < now) {
            remove(key);
            bson_destroy(doc);
            return StorageResult::failure("key expired");
        }
    }

    StorageResult ret = StorageResult::success(value);
    bson_destroy(doc);
    return ret;
#else
    (void)key;
    return StorageResult::failure("MongodbStorage not built with MongoDB support");
#endif
}

StorageResult MongodbStorage::put(const std::string& key, const std::string& value,
                                    std::int64_t expire_at) {
#if defined(CHWELL_USE_MONGODB)
    if (!collection_) return StorageResult::failure("not connected");

    mongoc_collection_t* coll = static_cast<mongoc_collection_t*>(collection_);
    bson_t* doc = bson_new();
    BSON_APPEND_UTF8(doc, "_id", key.c_str());
    BSON_APPEND_UTF8(doc, "v", value.c_str());
    BSON_APPEND_INT64(doc, "expire_at", expire_at);

    bson_t* query = bson_new();
    BSON_APPEND_UTF8(query, "_id", key.c_str());

    bson_t* opts = bson_new();
    BSON_APPEND_BOOL(opts, "upsert", true);

    bson_error_t error;
    bool ok = mongoc_collection_replace_one(coll, query, doc, opts, nullptr, &error);
    bson_destroy(opts);
    bson_destroy(query);
    bson_destroy(doc);

    if (!ok) {
        return StorageResult::failure(error.message);
    }
    return StorageResult::success();
#else
    (void)key;
    (void)value;
    (void)expire_at;
    return StorageResult::failure("MongodbStorage not built with MongoDB support");
#endif
}

StorageResult MongodbStorage::remove(const std::string& key) {
#if defined(CHWELL_USE_MONGODB)
    if (!collection_) return StorageResult::failure("not connected");

    mongoc_collection_t* coll = static_cast<mongoc_collection_t*>(collection_);
    bson_t* query = bson_new();
    BSON_APPEND_UTF8(query, "_id", key.c_str());

    bson_error_t error;
    bool ok = mongoc_collection_delete_one(coll, query, nullptr, nullptr, &error);
    bson_destroy(query);

    if (!ok) {
        return StorageResult::failure(error.message);
    }
    return StorageResult::success();
#else
    (void)key;
    return StorageResult::failure("MongodbStorage not built with MongoDB support");
#endif
}

bool MongodbStorage::exists(const std::string& key) {
#if defined(CHWELL_USE_MONGODB)
    if (!collection_) return false;

    mongoc_collection_t* coll = static_cast<mongoc_collection_t*>(collection_);
    bson_t* query = bson_new();
    BSON_APPEND_UTF8(query, "_id", key.c_str());

    bson_t* opts = bson_new();
    bson_t projection;
    bson_init(&projection);
    BSON_APPEND_INT32(&projection, "expire_at", 1);
    BSON_APPEND_DOCUMENT(opts, "projection", &projection);
    bson_destroy(&projection);

    bson_error_t error;
    bson_t* doc =
        mongoc_collection_find_one_with_opts(coll, query, opts, nullptr, &error);
    bson_destroy(opts);
    bson_destroy(query);

    if (!doc) {
        (void)error;
        return false;
    }

    std::int64_t expire_at = 0;
    bson_iter_t iter;
    if (bson_iter_init(&iter, doc)) {
        while (bson_iter_next(&iter)) {
            const char* kn = bson_iter_key(&iter);
            if (std::strcmp(kn, "expire_at") == 0 && BSON_ITER_HOLDS_INT64(&iter)) {
                expire_at = bson_iter_int64(&iter);
            }
        }
    }
    bson_destroy(doc);

    if (expire_at > 0) {
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        if (expire_at < now) {
            return false;
        }
    }
    return true;
#else
    (void)key;
    return false;
#endif
}

std::vector<std::string> MongodbStorage::keys(const std::string& prefix) {
#if defined(CHWELL_USE_MONGODB)
    if (!collection_) return {};

    mongoc_collection_t* coll = static_cast<mongoc_collection_t*>(collection_);
    bson_t* query = bson_new();
    if (!prefix.empty()) {
        // 转义正则特殊字符，确保 prefix 作为字面量匹配
        std::string escaped;
        escaped.reserve(prefix.size() * 2 + 1);
        for (unsigned char c : prefix) {
            if (c == '.' || c == '*' || c == '+' || c == '?' || c == '(' ||
                c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
                c == '^' || c == '$' || c == '|' || c == '\\') {
                escaped += '\\';
            }
            escaped += static_cast<char>(c);
        }
        std::string pattern = "^" + escaped;
        BSON_APPEND_REGEX(query, "_id", pattern.c_str(), "");
    }

    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(
        coll, query, nullptr, nullptr);
    bson_destroy(query);

    std::vector<std::string> result;
    const bson_t* doc;
    while (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        if (bson_iter_init(&iter, doc) && bson_iter_find(&iter, "_id") &&
            BSON_ITER_HOLDS_UTF8(&iter)) {
            uint32_t len;
            const char* str = bson_iter_utf8(&iter, &len);
            result.push_back(std::string(str, len));
        }
    }
    bson_error_t cerr;
    if (mongoc_cursor_error(cursor, &cerr)) {
        (void)cerr;
        result.clear();
    }
    mongoc_cursor_destroy(cursor);
    return result;
#else
    (void)prefix;
    return {};
#endif
}

}  // namespace storage
}  // namespace chwell
