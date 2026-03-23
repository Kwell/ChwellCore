#pragma once

#include <functional>
#include <future>
#include <string>
#include <vector>

#include "chwell/storage/storage_types.h"

namespace chwell {
namespace storage {

// 异步存储回调类型
using AsyncCallback       = std::function<void(StorageResult)>;
using AsyncExistsCallback = std::function<void(bool)>;

// AsyncStorageInterface：异步存储接口
// 提供 Future 风格（返回 std::future）和 Callback 风格（接受回调函数）两套 API。
// 实现类须保证所有方法线程安全。
class AsyncStorageInterface {
public:
    virtual ~AsyncStorageInterface() = default;

    // -----------------------------------------------------------------------
    // Future 风格：适合 co_await / .get() / std::async 链式调用
    // -----------------------------------------------------------------------

    virtual std::future<StorageResult> async_get(const std::string& key) = 0;

    virtual std::future<StorageResult> async_put(const std::string& key,
                                                  const std::string& value,
                                                  std::int64_t expire_at = 0) = 0;

    virtual std::future<StorageResult> async_remove(const std::string& key) = 0;

    virtual std::future<bool> async_exists(const std::string& key) = 0;

    // 批量获取（顺序与 keys 一一对应）
    virtual std::future<std::vector<StorageResult>> async_mget(
        const std::vector<std::string>& keys) = 0;

    // 批量写入（全部成功返回 ok，遇到第一个失败即停止并返回该错误）
    virtual std::future<StorageResult> async_mput(
        const std::vector<StorageDocument>& docs) = 0;

    // -----------------------------------------------------------------------
    // Callback 风格：适合事件驱动 / 非阻塞场景
    // -----------------------------------------------------------------------

    virtual void async_get(const std::string& key, AsyncCallback cb) = 0;

    virtual void async_put(const std::string& key, const std::string& value,
                           AsyncCallback cb, std::int64_t expire_at = 0) = 0;

    virtual void async_remove(const std::string& key, AsyncCallback cb) = 0;

    virtual void async_exists(const std::string& key, AsyncExistsCallback cb) = 0;
};

}  // namespace storage
}  // namespace chwell
