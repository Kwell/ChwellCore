#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "chwell/storage/async_storage_adapter.h"
#include "chwell/storage/async_storage_interface.h"
#include "chwell/storage/memory_storage.h"
#include "chwell/storage/orm/document.h"
#include "chwell/storage/orm/entity.h"
#include "chwell/storage/orm/repository.h"
#include "chwell/storage/storage_component.h"
#include "chwell/storage/storage_config_loader.h"
#include "chwell/storage/storage_factory.h"
#include "chwell/storage/storage_interface.h"
#include "chwell/storage/storage_types.h"

using namespace chwell;
using namespace std::chrono_literals;

// ===========================================================================
// Document 序列化往返
// ===========================================================================

TEST(DocumentTest, RoundTripBasicTypes) {
    storage::orm::Document doc;
    doc.set_string("name", "alice");
    doc.set_int("level", 42);
    doc.set_int64("score", 9876543210LL);
    doc.set_double("ratio", 3.14);
    doc.set_bool("active", true);

    std::string s = doc.to_string();
    ASSERT_FALSE(s.empty());

    storage::orm::Document doc2;
    ASSERT_TRUE(doc2.from_string(s));

    EXPECT_EQ(doc2.get_string("name"), "alice");
    EXPECT_EQ(doc2.get_int("level"), 42);
    EXPECT_EQ(doc2.get_int64("score"), 9876543210LL);
    EXPECT_DOUBLE_EQ(doc2.get_double("ratio"), doc.get_double("ratio"));
    EXPECT_TRUE(doc2.get_bool("active"));
}

TEST(DocumentTest, RoundTripSpecialCharsInValue) {
    storage::orm::Document doc;
    // 值中含换行、等号、反斜杠
    doc.set_string("desc", "line1\nline2=ok\\end");
    std::string s = doc.to_string();

    storage::orm::Document doc2;
    ASSERT_TRUE(doc2.from_string(s));
    EXPECT_EQ(doc2.get_string("desc"), "line1\nline2=ok\\end");
}

TEST(DocumentTest, DefaultValues) {
    storage::orm::Document doc;
    EXPECT_EQ(doc.get_string("missing", "default"), "default");
    EXPECT_EQ(doc.get_int("missing", -1), -1);
    EXPECT_FALSE(doc.get_bool("missing", false));
    EXPECT_DOUBLE_EQ(doc.get_double("missing", 1.5), 1.5);
}

TEST(DocumentTest, HasAndClear) {
    storage::orm::Document doc;
    doc.set_string("k", "v");
    EXPECT_TRUE(doc.has("k"));
    EXPECT_FALSE(doc.has("other"));
    doc.clear();
    EXPECT_FALSE(doc.has("k"));
}

// ===========================================================================
// MemoryStorage 基本 CRUD
// ===========================================================================

TEST(MemoryStorageTest, BasicCrud) {
    storage::MemoryStorage s;
    EXPECT_FALSE(s.exists("k1"));

    auto r = s.put("k1", "hello");
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(s.exists("k1"));

    auto g = s.get("k1");
    ASSERT_TRUE(g.ok);
    EXPECT_EQ(g.value, "hello");

    r = s.remove("k1");
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(s.exists("k1"));

    auto g2 = s.get("k1");
    EXPECT_FALSE(g2.ok);
}

TEST(MemoryStorageTest, RemoveNonExistent) {
    storage::MemoryStorage s;
    auto r = s.remove("no_such_key");
    EXPECT_FALSE(r.ok);
}

// ===========================================================================
// MemoryStorage TTL 过期
// ===========================================================================

TEST(MemoryStorageTest, TtlExpiry) {
    storage::MemoryStorage s;
    // 过期时间：当前时间 - 1 秒（已过期）
    auto past = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count() - 1;
    s.put("expired_key", "value", past);

    // 已过期，应获取失败
    EXPECT_FALSE(s.exists("expired_key"));
    auto r = s.get("expired_key");
    EXPECT_FALSE(r.ok);
}

TEST(MemoryStorageTest, TtlNotYetExpired) {
    storage::MemoryStorage s;
    // 过期时间：当前时间 + 60 秒
    auto future_ts = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count() + 60;
    s.put("live_key", "alive", future_ts);

    EXPECT_TRUE(s.exists("live_key"));
    auto r = s.get("live_key");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value, "alive");
}

TEST(MemoryStorageTest, TtlZeroNeverExpires) {
    storage::MemoryStorage s;
    s.put("forever", "data", 0);  // expire_at=0 永不过期
    EXPECT_TRUE(s.exists("forever"));
    EXPECT_TRUE(s.get("forever").ok);
}

// ===========================================================================
// MemoryStorage 批量操作 mget / mput
// ===========================================================================

TEST(MemoryStorageTest, MgetMultipleKeys) {
    storage::MemoryStorage s;
    s.put("a", "1");
    s.put("b", "2");

    auto results = s.mget({"a", "b", "c"});
    ASSERT_EQ(results.size(), 3u);
    EXPECT_TRUE(results[0].ok);
    EXPECT_EQ(results[0].value, "1");
    EXPECT_TRUE(results[1].ok);
    EXPECT_EQ(results[1].value, "2");
    EXPECT_FALSE(results[2].ok);  // "c" 不存在
}

TEST(MemoryStorageTest, MputBatch) {
    storage::MemoryStorage s;
    std::vector<storage::StorageDocument> docs = {
        {"x", "10"},
        {"y", "20"},
        {"z", "30"},
    };
    auto r = s.mput(docs);
    EXPECT_TRUE(r.ok);

    EXPECT_EQ(s.get("x").value, "10");
    EXPECT_EQ(s.get("y").value, "20");
    EXPECT_EQ(s.get("z").value, "30");
}

// ===========================================================================
// MemoryStorage keys 前缀过滤
// ===========================================================================

TEST(MemoryStorageTest, KeysWithPrefix) {
    storage::MemoryStorage s;
    s.put("player:1", "a");
    s.put("player:2", "b");
    s.put("room:1", "c");

    auto player_keys = s.keys("player:");
    ASSERT_EQ(player_keys.size(), 2u);

    auto all_keys = s.keys("");
    EXPECT_EQ(all_keys.size(), 3u);
}

// ===========================================================================
// StorageConfigLoader
// ===========================================================================

TEST(StorageConfigLoaderTest, LoadFromString) {
    // host/port/database 在 mysql 子节点下，type=mysql 时才会被读取
    const char* yaml_content =
        "storage:\n"
        "  type: mysql\n"
        "  mysql:\n"
        "    host: 127.0.0.1\n"
        "    port: 3306\n"
        "    database: mydb\n"
        "    user: root\n";

    storage::StorageConfig cfg;
    bool ok = storage::StorageConfigLoader::load_from_string(yaml_content, cfg);
#if defined(CHWELL_USE_YAML)
    ASSERT_TRUE(ok);
    EXPECT_EQ(cfg.type, "mysql");
    EXPECT_EQ(cfg.host, "127.0.0.1");
    EXPECT_EQ(cfg.port, 3306);
    EXPECT_EQ(cfg.database, "mydb");
    EXPECT_EQ(cfg.user, "root");
#else
    // 没有 YAML 支持时应返回 false
    EXPECT_FALSE(ok);
#endif
}

TEST(StorageConfigLoaderTest, LoadMemoryTypeFromString) {
    const char* yaml_content =
        "storage:\n"
        "  type: memory\n";

    storage::StorageConfig cfg;
    bool ok = storage::StorageConfigLoader::load_from_string(yaml_content, cfg);
#if defined(CHWELL_USE_YAML)
    ASSERT_TRUE(ok);
    EXPECT_EQ(cfg.type, "memory");
#else
    EXPECT_FALSE(ok);
#endif
}

TEST(StorageConfigLoaderTest, LoadFromStringInvalidYaml) {
    storage::StorageConfig cfg;
    // 无效 YAML 不能成功加载
    bool ok = storage::StorageConfigLoader::load_from_string("not valid yaml: :", cfg);
    // 具体结果取决于是否有 YAML 支持；但不应崩溃
    (void)ok;
}

// ===========================================================================
// StorageFactory
// ===========================================================================

TEST(StorageFactoryTest, CreateMemoryByConfig) {
    storage::StorageConfig cfg;
    cfg.type = "memory";
    auto store = storage::StorageFactory::create(cfg);
    ASSERT_NE(store, nullptr);

    auto r = store->put("test", "val");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(store->get("test").value, "val");
}

TEST(StorageFactoryTest, CreateMemoryByTypeString) {
    auto store = storage::StorageFactory::create("memory");
    ASSERT_NE(store, nullptr);
    EXPECT_TRUE(store->put("k", "v").ok);
}

TEST(StorageFactoryTest, CreateUnknownTypeReturnsNull) {
    storage::StorageConfig cfg;
    cfg.type = "unknown_backend";
    auto store = storage::StorageFactory::create(cfg);
    EXPECT_EQ(store, nullptr);
}

// ===========================================================================
// StorageComponent
// ===========================================================================

TEST(StorageComponentTest, ConstructFromUniquePtr) {
    auto mem = std::unique_ptr<storage::StorageInterface>(
        new storage::MemoryStorage());
    storage::StorageComponent comp(std::move(mem));

    EXPECT_NE(comp.storage(), nullptr);
    auto r = comp.put("key", "val");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(comp.get("key").value, "val");
}

TEST(StorageComponentTest, ConstructFromConfig) {
    storage::StorageConfig cfg;
    cfg.type = "memory";
    storage::StorageComponent comp(cfg);
    EXPECT_NE(comp.storage(), nullptr);
    EXPECT_TRUE(comp.put("a", "b").ok);
}

TEST(StorageComponentTest, NullStorageGuardOnGet) {
    // 构造空 component（不传 storage）
    storage::StorageComponent comp(
        std::unique_ptr<storage::StorageInterface>(nullptr));

    auto r = comp.get("k");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error_msg.empty());
}

TEST(StorageComponentTest, NullStorageGuardOnRepository) {
    storage::StorageComponent comp(
        std::unique_ptr<storage::StorageInterface>(nullptr));

    // repository() 应抛出而非崩溃
    EXPECT_THROW(comp.repository<storage::orm::Entity>("tbl"),
                 std::runtime_error);
}

// ===========================================================================
// AsyncStorageAdapter — Future 风格
// ===========================================================================

TEST(AsyncStorageAdapterTest, AsyncGetPutRemove) {
    storage::MemoryStorage mem;
    storage::AsyncStorageAdapter adapter(&mem, 2);

    auto f_put = adapter.async_put("k1", "hello");
    ASSERT_TRUE(f_put.get().ok);

    auto f_get = adapter.async_get("k1");
    auto result = f_get.get();
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.value, "hello");

    auto f_exists = adapter.async_exists("k1");
    EXPECT_TRUE(f_exists.get());

    auto f_remove = adapter.async_remove("k1");
    EXPECT_TRUE(f_remove.get().ok);

    auto f_exists2 = adapter.async_exists("k1");
    EXPECT_FALSE(f_exists2.get());
}

TEST(AsyncStorageAdapterTest, AsyncMgetMput) {
    storage::MemoryStorage mem;
    storage::AsyncStorageAdapter adapter(&mem, 2);

    std::vector<storage::StorageDocument> docs = {
        {"p", "1"}, {"q", "2"}, {"r", "3"}};
    auto f_mput = adapter.async_mput(docs);
    ASSERT_TRUE(f_mput.get().ok);

    auto f_mget = adapter.async_mget({"p", "q", "r", "missing"});
    auto results = f_mget.get();
    ASSERT_EQ(results.size(), 4u);
    EXPECT_EQ(results[0].value, "1");
    EXPECT_EQ(results[1].value, "2");
    EXPECT_EQ(results[2].value, "3");
    EXPECT_FALSE(results[3].ok);
}

// ===========================================================================
// AsyncStorageAdapter — Callback 风格
// ===========================================================================

TEST(AsyncStorageAdapterTest, CallbackPutAndGet) {
    storage::MemoryStorage mem;
    storage::AsyncStorageAdapter adapter(&mem, 2);

    std::promise<storage::StorageResult> p_put;
    adapter.async_put("cb_key", "cb_val",
                      [&p_put](storage::StorageResult r) {
                          p_put.set_value(r);
                      });
    ASSERT_TRUE(p_put.get_future().get().ok);

    std::promise<storage::StorageResult> p_get;
    adapter.async_get("cb_key",
                      [&p_get](storage::StorageResult r) {
                          p_get.set_value(r);
                      });
    auto res = p_get.get_future().get();
    ASSERT_TRUE(res.ok);
    EXPECT_EQ(res.value, "cb_val");
}

TEST(AsyncStorageAdapterTest, CallbackExists) {
    storage::MemoryStorage mem;
    mem.put("exist_key", "v");
    storage::AsyncStorageAdapter adapter(&mem, 1);

    std::promise<bool> p;
    adapter.async_exists("exist_key", [&p](bool v) { p.set_value(v); });
    EXPECT_TRUE(p.get_future().get());
}

TEST(AsyncStorageAdapterTest, CallbackRemove) {
    storage::MemoryStorage mem;
    mem.put("rm_key", "v");
    storage::AsyncStorageAdapter adapter(&mem, 2);

    std::promise<storage::StorageResult> p;
    adapter.async_remove("rm_key",
                         [&p](storage::StorageResult r) { p.set_value(r); });
    EXPECT_TRUE(p.get_future().get().ok);
    EXPECT_FALSE(mem.exists("rm_key"));
}

// ===========================================================================
// AsyncStorageAdapter — 并发写入一致性
// ===========================================================================

TEST(AsyncStorageAdapterTest, ConcurrentWrites) {
    storage::MemoryStorage mem;
    storage::AsyncStorageAdapter adapter(&mem, 4);

    const int N = 100;
    std::vector<std::future<storage::StorageResult>> futures;
    futures.reserve(N);

    for (int i = 0; i < N; ++i) {
        std::string key   = "cw_key_" + std::to_string(i);
        std::string value = "val_"    + std::to_string(i);
        futures.push_back(adapter.async_put(key, value));
    }

    int ok_count = 0;
    for (auto& f : futures) {
        if (f.get().ok) ++ok_count;
    }
    EXPECT_EQ(ok_count, N);

    // 验证所有 key 均可读取
    for (int i = 0; i < N; ++i) {
        std::string key      = "cw_key_" + std::to_string(i);
        std::string expected = "val_"    + std::to_string(i);
        auto r = mem.get(key);
        ASSERT_TRUE(r.ok) << "key=" << key;
        EXPECT_EQ(r.value, expected);
    }
}
