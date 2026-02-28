// ORM 使用示例：通过类型安全的 Repository 访问存储，无需手写 key-value
#include <iostream>
#include <memory>

#include "chwell/core/logger.h"
#include "chwell/storage/storage_factory.h"
#include "chwell/storage/storage_component.h"
#include "chwell/storage/orm/entity.h"
#include "chwell/storage/orm/document.h"
#include "chwell/storage/orm/repository.h"

using namespace chwell;

// 定义 Player 实体：继承 Entity，实现 to_document/from_document
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
        level_ = doc.get_int("level", 0);
    }
};

int main() {
    CHWELL_LOG_INFO("ORM Example: type-safe storage access");

    // 从 YAML 创建存储，失败则回退到 memory
    auto store = storage::StorageFactory::create_from_yaml("config/storage.yaml");
    if (!store) {
        storage::StorageConfig config;
        config.type = "memory";
        store = storage::StorageFactory::create(config);
    }
    storage::StorageComponent component(std::move(store));

    // 通过 Repository 访问，无需手写 key-value
    auto repo = component.repository<Player>("players");

    // 保存
    Player player("p001", "alice", 10);
    auto r = repo.save(player);
    if (!r.ok) {
        std::cerr << "save failed: " << r.error_msg << std::endl;
        return 1;
    }
    std::cout << "save ok" << std::endl;

    // 查找
    Player found;
    if (repo.find("p001", found)) {
        std::cout << "find ok: id=" << found.id_ << ", name=" << found.name_
                  << ", level=" << found.level_ << std::endl;
    } else {
        std::cout << "find: not found" << std::endl;
    }

    // 或使用 unique_ptr
    auto p = repo.find("p001");
    if (p) {
        std::cout << "find (ptr): name=" << p->name_ << std::endl;
    }

    // 更新
    player.level_ = 15;
    repo.save(player);
    repo.find("p001", found);
    std::cout << "after update: level=" << found.level_ << std::endl;

    // 列出所有
    auto all = repo.find_all();
    std::cout << "find_all: count=" << all.size() << std::endl;

    // 删除
    repo.remove("p001");
    std::cout << "exists after remove: " << (repo.exists("p001") ? "yes" : "no")
              << std::endl;

    // 同一 component 可创建多个 Repository
    Player p2("p002", "bob", 5);
    repo.save(p2);
    std::cout << "saved p002" << std::endl;

    CHWELL_LOG_INFO("ORM example done");
    return 0;
}
