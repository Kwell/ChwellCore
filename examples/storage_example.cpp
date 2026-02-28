// 存储组件使用示例：上层逻辑不关心底层是 memory、mysql 还是 mongodb
// 介质和连接配置通过 config/storage.yaml 统一控制
#include <iostream>
#include <memory>

#include "chwell/core/logger.h"
#include "chwell/storage/storage_interface.h"
#include "chwell/storage/storage_types.h"
#include "chwell/storage/storage_factory.h"
#include "chwell/storage/storage_component.h"

using namespace chwell;

void demo_storage(storage::StorageInterface* store) {
    if (!store) {
        std::cerr << "Storage not initialized" << std::endl;
        return;
    }

    // 上层逻辑统一使用 StorageInterface，不关心底层介质
    auto r = store->put("player:123", R"({"name":"alice","level":10})");
    if (!r.ok) {
        std::cerr << "put failed: " << r.error_msg << std::endl;
        return;
    }
    std::cout << "put ok" << std::endl;

    r = store->get("player:123");
    if (r.ok) {
        std::cout << "get ok: " << r.value << std::endl;
    } else {
        std::cerr << "get failed: " << r.error_msg << std::endl;
    }

    std::cout << "exists: " << (store->exists("player:123") ? "yes" : "no")
              << std::endl;

    r = store->remove("player:123");
    std::cout << "remove: " << (r.ok ? "ok" : r.error_msg) << std::endl;

    std::cout << "exists after remove: "
              << (store->exists("player:123") ? "yes" : "no") << std::endl;
}

int main() {
    CHWELL_LOG_INFO("Storage Example: upper logic independent of backend");

    // 方式 1：从 YAML 配置创建（推荐，介质和连接参数均在 storage.yaml 中）
    auto store = storage::StorageFactory::create_from_yaml("config/storage.yaml");
    if (!store) {
        std::cerr << "Failed to create storage from YAML, fallback to memory" << std::endl;
        storage::StorageConfig config;
        config.type = "memory";
        store = storage::StorageFactory::create(config);
    }
    std::cout << "=== Storage from YAML ===" << std::endl;
    demo_storage(store.get());

    // 方式 2：通过 StorageComponent 挂载到 Service
    storage::StorageComponent component("config/storage.yaml");
    if (component.storage()) {
        std::cout << "\n=== StorageComponent (from YAML) ===" << std::endl;
        demo_storage(component.storage());
    }

    CHWELL_LOG_INFO("Storage example done");
    return 0;
}
