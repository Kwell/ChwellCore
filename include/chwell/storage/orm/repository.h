#pragma once

#include <string>
#include <vector>
#include <memory>

#include "chwell/storage/storage_interface.h"
#include "chwell/storage/storage_types.h"
#include "chwell/storage/orm/entity.h"

namespace chwell {
namespace storage {
namespace orm {

// Repository<T>：ORM 仓储，提供类型安全的 CRUD
// 上层通过 repository.save(player) 等访问，无需手写 key-value
template <typename T>
class Repository {
public:
    Repository(StorageInterface* storage, const std::string& table_name)
        : storage_(storage), table_name_(table_name) {}

    // 保存（插入或更新）
    StorageResult save(const T& entity) {
        std::string key = make_key(entity.id());
        std::string value = entity.to_document().to_string();
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
    StorageResult remove(const std::string& id) {
        return storage_->remove(make_key(id));
    }

    // 检查是否存在
    bool exists(const std::string& id) {
        return storage_->exists(make_key(id));
    }

    // 列出所有（按前缀查询）
    std::vector<std::unique_ptr<T>> find_all() {
        std::vector<std::unique_ptr<T>> result;
        std::string prefix = table_name_ + ":";
        auto keys = storage_->keys(prefix);
        for (const auto& k : keys) {
            auto entity = find(extract_id(k));
            if (entity) {
                result.push_back(std::move(entity));
            }
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
