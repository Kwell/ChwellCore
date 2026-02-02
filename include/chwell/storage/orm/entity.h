#pragma once

#include <string>

#include "chwell/storage/orm/document.h"

namespace chwell {
namespace storage {
namespace orm {

// Entity：ORM 实体基类，派生类定义表名和字段映射
// 上层通过继承 Entity 定义业务模型，通过 Repository 访问，无需关心底层存储
class Entity {
public:
    virtual ~Entity() {}

    // 表/集合名（如 "players"、"rooms"）
    virtual std::string table_name() const = 0;

    // 主键 ID
    virtual std::string id() const = 0;

    // 实体 <-> Document 转换（派生类实现字段映射）
    virtual Document to_document() const = 0;
    virtual void from_document(const Document& doc) = 0;
};

}  // namespace orm
}  // namespace storage
}  // namespace chwell
