#pragma once

#include <cassert>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "chwell/core/logger.h"
#include "chwell/storage/orm/entity.h"
#include "chwell/storage/storage_interface.h"
#include "chwell/storage/storage_types.h"

namespace chwell {
namespace storage {
namespace orm {

// Repository<T>：ORM 仓储，提供类型安全的 CRUD。
// 键前缀为 "<table_name>:"，与 entity.table_name() 必须一致（save 时校验）。
// find_all() 遍历前缀下全部 key，顺序由底层实现决定，不保证稳定；某条记录 get 失败或
// 反序列化失败时会跳过并打 WARN 日志（与「空表」不同，空表返回空 vector 且无日志）。
template <typename T>
class Repository {
    static_assert(std::is_base_of_v<Entity, T> && std::is_default_constructible_v<T>,
                  "Repository<T>: T must be a concrete class derived from Entity");

public:
    Repository(StorageInterface* storage, const std::string& table_name)
        : storage_(storage), table_name_(table_name) {
        assert(storage != nullptr && "Repository: StorageInterface must not be null");
    }

    // 保存（插入或更新）；写入前将 Document 中 "id" 设为 entity.id()，与存储键一致
    [[nodiscard]] StorageResult save(const T& entity) {
        if (entity.table_name() != table_name_) {
            return StorageResult::failure(
                "ORM Repository: entity.table_name() does not match repository table_name");
        }
        Document doc = entity.to_document();
        doc.set_string("id", entity.id());
        std::string key = make_key(entity.id());
        std::string value = doc.to_string();
        return storage_->put(key, value);
    }

    // 按 ID 查找
    bool find(const std::string& id, T& out) {
        std::string key = make_key(id);
        auto r = storage_->get(key);
        if (!r.ok) return false;
        Document doc;
        if (!doc.from_string(r.value)) return false;
        out.from_document(doc);
        return true;
    }

    // 按 ID 查找，返回 unique_ptr（nullptr 表示未找到）
    std::unique_ptr<T> find(const std::string& id) {
        std::unique_ptr<T> entity(new T());
        if (find(id, *entity)) {
            return entity;
        }
        return nullptr;
    }

    // 删除
    [[nodiscard]] StorageResult remove(const std::string& id) {
        return storage_->remove(make_key(id));
    }

    // 检查是否存在
    bool exists(const std::string& id) {
        return storage_->exists(make_key(id));
    }

    // 列出前缀 table_name: 下的全部实体（无序；部分失败时跳过并打日志，见类注释）
    std::vector<std::unique_ptr<T>> find_all() {
        std::vector<std::unique_ptr<T>> result;
        std::string prefix = table_name_ + ":";
        auto keys = storage_->keys(prefix);
        if (keys.empty()) {
            return result;
        }
        auto responses = storage_->mget(keys);
        if (responses.size() != keys.size()) {
            CHWELL_LOG_WARN("ORM find_all: mget size mismatch (keys="
                            << keys.size() << " responses=" << responses.size()
                            << "), using per-key get");
            for (const auto& k : keys) {
                auto entity = find(extract_id(k));
                if (entity) {
                    result.push_back(std::move(entity));
                } else {
                    CHWELL_LOG_WARN("ORM find_all: skip key '" << k << "' (get or parse failed)");
                }
            }
            return result;
        }
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (!responses[i].ok) {
                CHWELL_LOG_WARN("ORM find_all: skip key '" << keys[i] << "' (mget entry not ok)");
                continue;
            }
            Document doc;
            if (!doc.from_string(responses[i].value)) {
                CHWELL_LOG_WARN("ORM find_all: skip key '" << keys[i] << "' (document parse failed)");
                continue;
            }
            auto entity = std::unique_ptr<T>(new T());
            entity->from_document(doc);
            result.push_back(std::move(entity));
        }
        return result;
    }

private:
    std::string make_key(const std::string& id) const {
        return table_name_ + ":" + id;
    }

    std::string extract_id(const std::string& key) const {
        const std::string prefix = table_name_ + ":";
        if (key.compare(0, prefix.size(), prefix) == 0) {
            return key.substr(prefix.size());
        }
        return key;
    }

    StorageInterface* storage_;
    std::string table_name_;
};

}  // namespace orm
}  // namespace storage
}  // namespace chwell
