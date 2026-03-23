#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "chwell/storage/memory_storage.h"
#include "chwell/storage/orm/entity.h"
#include "chwell/storage/orm/document.h"
#include "chwell/storage/orm/repository.h"

using namespace chwell;

namespace {

struct Player : public storage::orm::Entity {
    std::string id_;
    std::string name_;
    int level_{0};

    Player() = default;
    Player(const std::string& id, const std::string& name, int level)
        : id_(id), name_(name), level_(level) {}

    std::string table_name() const override { return "players"; }
    std::string id() const override { return id_; }

    storage::orm::Document to_document() const override {
        storage::orm::Document doc;
        doc.set_string("id", id_);
        doc.set_string("name", name_);
        doc.set_int("level", level_);
        return doc;
    }

    void from_document(const storage::orm::Document& doc) override {
        id_ = doc.get_string("id");
        name_ = doc.get_string("name");
        level_ = doc.get_int("level");
    }
};

}  // namespace

TEST(OrmRepositoryTest, SaveFailsWhenTableNameMismatch) {
    storage::MemoryStorage storage;
    storage::orm::Repository<Player> repo(&storage, "wrong_table");
    Player p("p001", "alice", 10);
    auto r = repo.save(p);
    EXPECT_FALSE(r.ok);
}

TEST(OrmRepositoryTest, CrudFlowOnMemoryStorage) {
    storage::MemoryStorage storage;
    storage::orm::Repository<Player> repo(&storage, "players");

    Player p1("p001", "alice", 10);

    auto r = repo.save(p1);
    ASSERT_TRUE(r.ok);

    EXPECT_TRUE(repo.exists("p001"));

    Player found;
    ASSERT_TRUE(repo.find("p001", found));
    EXPECT_EQ("p001", found.id_);
    EXPECT_EQ("alice", found.name_);
    EXPECT_EQ(10, found.level_);

    auto ptr = repo.find("p001");
    ASSERT_NE(nullptr, ptr);
    EXPECT_EQ("alice", ptr->name_);

    p1.level_ = 15;
    r = repo.save(p1);
    ASSERT_TRUE(r.ok);

    ASSERT_TRUE(repo.find("p001", found));
    EXPECT_EQ(15, found.level_);

    auto all = repo.find_all();
    ASSERT_EQ(1u, all.size());
    EXPECT_EQ("p001", all[0]->id_);

    r = repo.remove("p001");
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(repo.exists("p001"));
}

