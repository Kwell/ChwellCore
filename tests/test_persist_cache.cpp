#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "chwell/storage/memory_storage.h"
#include "chwell/storage/orm/document.h"
#include "chwell/storage/orm/repository.h"
#include "chwell/storage/orm/writeback_cache.h"

using namespace chwell;
using namespace chwell::storage;
using namespace chwell::storage::orm;

namespace {

class Player : public PersistableEntity {
public:
    std::string table_name() const override { return "players"; }
    std::string id() const override { return id_; }

    Document to_document() const override {
        Document d;
        d.set_string("id", id_);
        d.set_string("name", name_);
        d.set_int("level", level_);
        d.set_int("gold", gold_);
        return d;
    }

    void from_document(const Document& d) override {
        id_ = d.get_string("id");
        name_ = d.get_string("name");
        level_ = d.get_int("level", 1);
        gold_ = d.get_int("gold", 0);
        clear_dirty();
    }

    CHWELL_FIELD(std::string, id_, player_id)
    CHWELL_FIELD(std::string, name_, name)
    CHWELL_FIELD(int, level_, level)
    CHWELL_FIELD(int, gold_, gold)

    // id_ 的 setter 需要自定义：id 变了键就变了，简单场景直接赋值
    void set_id(const std::string& v) { id_ = v; }

private:
    std::string id_ = "p1";
    std::string name_ = "hero";
    int level_ = 1;
    int gold_ = 0;
};

}  // namespace

TEST(PersistableEntityTest, SetFieldMarksDirty) {
    Player p;
    EXPECT_FALSE(p.is_dirty());
    p.set_name("hero");  // 同值，不打脏
    EXPECT_FALSE(p.is_dirty());
    p.set_level(5);
    EXPECT_TRUE(p.is_field_dirty("level"));
    EXPECT_FALSE(p.is_field_dirty("name"));
    p.clear_dirty();
    EXPECT_FALSE(p.is_dirty());
}

TEST(PersistableEntityTest, DirtyDocumentOnlyContainsChanged) {
    Player p;
    p.set_gold(100);
    Document d = p.to_dirty_document();
    EXPECT_TRUE(d.has("gold"));
    EXPECT_FALSE(d.has("level"));
    EXPECT_FALSE(d.has("name"));
}

TEST(PersistableEntityTest, MutableRefMarksDirty) {
    Player p;
    p.mutable_gold() = 42;
    EXPECT_TRUE(p.is_field_dirty("gold"));
    EXPECT_EQ(42, p.gold());
}

TEST(WriteBackCacheTest, PutGetAndFlush) {
    MemoryStorage mem;
    Repository<Player> repo(&mem, "players");
    WriteBackCache<Player> cache(&repo);

    Player p;
    p.set_name("alice");
    p.set_level(10);
    ASSERT_TRUE(cache.put(p));
    EXPECT_EQ(1u, cache.size());
    EXPECT_EQ(1u, cache.dirty_count());

    // 未 flush 前，底层仍无数据
    EXPECT_FALSE(repo.exists("p1"));

    EXPECT_EQ(1u, cache.flush());
    EXPECT_EQ(0u, cache.dirty_count());
    EXPECT_TRUE(repo.exists("p1"));

    // 落盘后再读：缓存命中
    Player got;
    ASSERT_TRUE(cache.get("p1", got));
    EXPECT_EQ("alice", got.name());
    EXPECT_EQ(10, got.level());
    EXPECT_FALSE(got.is_dirty());  // 加载/缓存副本不应带脏标
}

TEST(WriteBackCacheTest, MissLoadsFromStorage) {
    MemoryStorage mem;
    Repository<Player> repo(&mem, "players");
    WriteBackCache<Player> cache(&repo);

    Player p;
    p.set_name("bob");
    p.set_level(3);
    ASSERT_TRUE(repo.save(p).ok);

    Player got;
    ASSERT_TRUE(cache.get("p1", got));
    EXPECT_EQ("bob", got.name());
    EXPECT_EQ(3, got.level());
    // 加载进缓存后不应计为脏
    EXPECT_EQ(0u, cache.dirty_count());
}

TEST(WriteBackCacheTest, InvalidateDropsWithoutWrite) {
    MemoryStorage mem;
    Repository<Player> repo(&mem, "players");
    WriteBackCache<Player> cache(&repo);

    Player p;
    p.set_name("carol");
    ASSERT_TRUE(cache.put(p));
    cache.invalidate("p1");
    EXPECT_EQ(0u, cache.size());
    EXPECT_FALSE(repo.exists("p1"));  // 未写回
}

TEST(WriteBackCacheTest, FlushAllWritesCleanEntries) {
    MemoryStorage mem;
    Repository<Player> repo(&mem, "players");
    WriteBackCache<Player> cache(&repo);

    Player p;
    p.set_name("dave");
    ASSERT_TRUE(cache.put(p));
    ASSERT_TRUE(cache.flush() > 0);
    EXPECT_EQ(0u, cache.dirty_count());

    // flush_all 对 clean 项也会强制落盘（底层被删后可补写）
    EXPECT_EQ(1u, cache.flush_all());
    EXPECT_TRUE(repo.exists("p1"));
}

TEST(WriteBackCacheTest, WriteThroughSavesImmediately) {
    MemoryStorage mem;
    Repository<Player> repo(&mem, "players");
    WriteBackCache<Player>::Options opt;
    opt.write_through_on_save = true;
    WriteBackCache<Player> cache(&repo, opt);

    Player p;
    p.set_name("eve");
    ASSERT_TRUE(cache.put(p));
    EXPECT_TRUE(repo.exists("p1"));
    EXPECT_EQ(0u, cache.dirty_count());
}

TEST(WriteBackCacheTest, EvictPrefersCleanEntries) {
    MemoryStorage mem;
    Repository<Player> repo(&mem, "players");
    WriteBackCache<Player>::Options opt;
    opt.max_entries = 2;
    WriteBackCache<Player> cache(&repo, opt);

    Player a;
    a.set_id("a");
    a.set_name("a");
    ASSERT_TRUE(cache.put(a));
    ASSERT_TRUE(cache.flush() > 0);  // a clean

    Player b;
    b.set_id("b");
    b.set_name("b");
    ASSERT_TRUE(cache.put(b));  // b dirty

    Player c;
    c.set_id("c");
    c.set_name("c");
    ASSERT_TRUE(cache.put(c));  // 触发淘汰，优先清 clean 的 a

    EXPECT_EQ(2u, cache.size());
    EXPECT_FALSE(cache.in_cache("a"));
    EXPECT_TRUE(cache.in_cache("b"));
    EXPECT_TRUE(cache.in_cache("c"));
    EXPECT_EQ(2u, cache.dirty_count());  // b、c 仍脏，未丢写回
}

TEST(WriteBackCacheTest, EntityDirtySurvivesSaveCopy) {
    // save 落盘时会 clear 脏标；缓存内实体仍应按 flush 成功后清脏
    MemoryStorage mem;
    Repository<Player> repo(&mem, "players");
    WriteBackCache<Player> cache(&repo);

    Player p;
    p.set_level(9);
    ASSERT_TRUE(cache.put(p));
    EXPECT_EQ(1u, cache.flush());
    Player got;
    ASSERT_TRUE(cache.get("p1", got));
    EXPECT_EQ(9, got.level());
    EXPECT_EQ(0u, cache.dirty_count());
}
