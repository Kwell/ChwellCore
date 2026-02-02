#pragma once

#include <string>

#include "chwell/storage/storage_types.h"

namespace chwell {
namespace storage {

// 从 YAML 文件加载存储配置
// 上层逻辑只需指定配置文件路径，不关心具体介质
class StorageConfigLoader {
public:
    // 从 YAML 文件加载，返回配置（失败时 ok=false）
    static bool load(const std::string& yaml_path, StorageConfig& out_config);

    // 从 YAML 字符串加载（用于测试或嵌入式配置）
    static bool load_from_string(const std::string& yaml_content,
                                 StorageConfig& out_config);
};

}  // namespace storage
}  // namespace chwell
