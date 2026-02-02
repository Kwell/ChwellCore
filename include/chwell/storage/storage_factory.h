#pragma once

#include <memory>
#include <string>

#include "chwell/storage/storage_interface.h"
#include "chwell/storage/storage_types.h"

namespace chwell {
namespace storage {

// 存储工厂：根据配置创建对应介质的存储实例
// 上层逻辑只需调用 create(config) 或 create_from_yaml(path)，不关心具体实现
class StorageFactory {
public:
    // 根据配置创建存储实例
    // type: memory | mysql | mongodb
    static std::unique_ptr<StorageInterface> create(const StorageConfig& config);

    // 从 YAML 配置文件创建（推荐）
    // 配置路径可通过环境变量 STORAGE_CONFIG 覆盖
    static std::unique_ptr<StorageInterface> create_from_yaml(
        const std::string& yaml_path = "config/storage.yaml");

    // 便捷方法：根据类型字符串创建
    static std::unique_ptr<StorageInterface> create(const std::string& type,
                                                     const std::string& host = "",
                                                     int port = 0,
                                                     const std::string& database = "");
};

}  // namespace storage
}  // namespace chwell
