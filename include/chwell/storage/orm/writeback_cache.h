#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "chwell/storage/orm/document.h"
#include "chwell/storage/orm/entity.h"
#include "chwell/storage/orm/repository.h"

namespace chwell {
namespace storage {
namespace orm {

// ========== 字段级脏标记 ==========
//
// PersistableEntity 在 Entity 之上增加「哪些字段被改过」的记录，供写回缓存做
// 增量落盘。标记由 set_field() 自动完成，也可手动 mark_dirty()。
//
// 字段注册（annotation）通过 PersistableEntity::bind_field 完成：把 C++ 成员
// 与 Document 键绑定，set 时自动打脏标。典型用法见类尾 CHWELL_FIELD 宏。

class PersistableEntity : public Entity {
public:
    void mark_dirty(const std::string& field) { dirty_fields_.insert(field); }

    void clear_dirty() { dirty_fields_.clear(); }

    bool is_dirty() const { return !dirty_fields_.empty(); }

    bool is_field_dirty(const std::string& field) const {
        return dirty_fields_.count(field) > 0;
    }

    const std::set<std::string>& dirty_fields() const { return dirty_fields_; }

    // 仅包含脏字段的 Document（部分更新用）
    Document to_dirty_document() const {
        Document full = to_document();
        Document part;
        for (const auto& f : dirty_fields_) {
            if (full.has(f)) {
                part.set_string(f, full.get_string(f));
            }
        }
        return part;
    }

    // 带脏标记的 setter：值不变不打标
    template <typename T>
    void set_field(T& member, const T& value, const std::string& key) {
        if (member == value) return;
        member = value;
        mark_dirty(key);
    }

protected:
    // 派生类在 from_document 后调用，清除加载带来的“脏”
    void clear_dirty_unlocked() { dirty_fields_.clear(); }

private:
    std::set<std::string> dirty_fields_;
};

// 便捷宏：在派生类中声明持久化字段并生成 getter/setter。
// 用法：
//   class Player : public PersistableEntity {
//   public:
//       std::string table_name() const override { return "players"; }
//       std::string id() const override { return id_; }
//       Document to_document() const override { ... }
//       void from_document(const Document& d) override { ... clear_dirty(); }
//       CHWELL_FIELD(std::string, name_, name)
//       CHWELL_FIELD(int, level_, level)
//   };
// set_name("x") / set_level(2) 会自动打脏标并更新成员。
#define CHWELL_FIELD(Type, member, key)                                  \
    const Type& key() const { return member; }                           \
    void set_##key(const Type& v) { set_field(member, v, #key); }        \
    Type& mutable_##key() { mark_dirty(#key); return member; }

// ========== 写回缓存 ==========
//
// WriteBackCache<T>：Repository 之上的业务层缓存。
// - get：先查内存，miss 再打存储并缓存
// - put/save：写入缓存并标记脏
// - flush()：把脏实体批量写回存储，成功后清除脏标
// - flush_all()：忽略脏标，全部写回
// - auto_flush_if_due()：按 interval 调用，适合挂在定时器上
// - invalidate：从缓存移除（不写回）
//
// 并发：内部 shared_mutex，find 返回副本/指针副本，调用方持有期间不会与 flush 互相破坏。

template <typename T>
class WriteBackCache {
    static_assert(std::is_base_of_v<PersistableEntity, T>,
                  "WriteBackCache<T>: T must derive from PersistableEntity");

public:
    struct Options {
        std::size_t max_entries = 10000;           // 0 表示不限
        std::int64_t flush_interval_ms = 5000;     // 自动写回间隔；0 关闭
        bool write_through_on_save = false;        // save 时立刻落盘
    };

    WriteBackCache(Repository<T>* repo, Options opt = Options())
        : repo_(repo), opt_(opt), last_flush_ms_(now_ms()) {}

    // 读：缓存优先
    bool get(const std::string& id, T& out) {
        {
            std::shared_lock lock(mu_);
            auto it = cache_.find(id);
            if (it != cache_.end()) {
                out = it->second;
                return true;
            }
        }
        std::unique_ptr<T> loaded = repo_->find(id);
        if (!loaded) return false;
        out = *loaded;
        {
            std::unique_lock lock(mu_);
            evict_if_needed_locked();
            cache_[id] = out;
        }
        return true;
    }

    // 写：进缓存并打脏
    // 返回 false 表示 write_through 且底层落盘失败
    bool put(const T& entity) {
        T copy = entity;
        copy.mark_dirty("__entity__");  // 记录级脏，即使字段集为空也需写回
        {
            std::unique_lock lock(mu_);
            evict_if_needed_locked();
            cache_[entity.id()] = copy;
        }
        if (opt_.write_through_on_save) {
            T tmp = entity;
            tmp.clear_dirty();
            auto r = repo_->save(tmp);
            if (!r.ok) return false;
            std::unique_lock lock(mu_);
            auto it = cache_.find(entity.id());
            if (it != cache_.end()) it->second.clear_dirty();
        }
        return true;
    }

    // 失效（不写回）
    void invalidate(const std::string& id) {
        std::unique_lock lock(mu_);
        cache_.erase(id);
    }

    void clear() {
        std::unique_lock lock(mu_);
        cache_.clear();
    }

    // 写回所有脏实体；返回成功条数
    std::size_t flush() {
        std::vector<std::pair<std::string, T>> dirty;
        {
            std::shared_lock lock(mu_);
            for (const auto& kv : cache_) {
                if (kv.second.is_dirty()) {
                    dirty.emplace_back(kv.first, kv.second);
                }
            }
        }
        std::size_t ok = 0;
        for (auto& [id, entity] : dirty) {
            // 带脏标保存前先克隆一份清掉脏标，避免 save 内部再次触发
            T to_save = entity;
            to_save.clear_dirty();
            auto r = repo_->save(to_save);
            if (!r.ok) {
                continue;
            }
            ++ok;
            std::unique_lock lock(mu_);
            auto it = cache_.find(id);
            if (it != cache_.end()) it->second.clear_dirty();
        }
        last_flush_ms_ = now_ms();
        return ok;
    }

    // 全部写回（含未打脏标但需持久化的）
    std::size_t flush_all() {
        {
            std::unique_lock lock(mu_);
            for (auto& kv : cache_) {
                kv.second.mark_dirty("__entity__");
            }
        }
        return flush();
    }

    // 定时调用：到达间隔则 flush；返回是否执行了写回
    bool auto_flush_if_due() {
        if (opt_.flush_interval_ms <= 0) return false;
        if (now_ms() - last_flush_ms_ < opt_.flush_interval_ms) return false;
        flush();
        return true;
    }

    std::size_t size() const {
        std::shared_lock lock(mu_);
        return cache_.size();
    }

    // 仅查内存缓存（不回源），便于测试淘汰策略
    bool in_cache(const std::string& id) const {
        std::shared_lock lock(mu_);
        return cache_.find(id) != cache_.end();
    }

    std::size_t dirty_count() const {
        std::shared_lock lock(mu_);
        std::size_t n = 0;
        for (const auto& kv : cache_) {
            if (kv.second.is_dirty()) ++n;
        }
        return n;
    }

    const Options& options() const { return opt_; }

private:
    void evict_if_needed_locked() {
        if (opt_.max_entries == 0) return;
        // 简单策略：满了先 flush 脏数据再整表清空最旧一半（按插入序 map 无法精确 LRU，
        // 用 unordered 则无序。这里用 std::map 按 key 排序做确定性淘汰，行为可测。）
        while (cache_.size() >= opt_.max_entries) {
            // 优先淘汰未脏项
            auto it = cache_.begin();
            for (auto i = cache_.begin(); i != cache_.end(); ++i) {
                if (!i->second.is_dirty()) {
                    it = i;
                    break;
                }
            }
            if (it->second.is_dirty()) {
                // 全是脏的：先写回再清空
                // 此处不能直接调 flush()（会拿 shared_lock），改为就地保存
                for (auto& kv : cache_) {
                    T to_save = kv.second;
                    to_save.clear_dirty();
                    repo_->save(to_save);
                }
                cache_.clear();
                return;
            }
            cache_.erase(it);
        }
    }

    static std::int64_t now_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    Repository<T>* repo_;
    Options opt_;
    std::int64_t last_flush_ms_;
    mutable std::shared_mutex mu_;
    std::map<std::string, T> cache_;
};

}  // namespace orm
}  // namespace storage
}  // namespace chwell
