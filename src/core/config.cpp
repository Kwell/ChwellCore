#include "chwell/core/config.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace core {

bool Config::load_from_file(const std::string& path) {
    // TODO: 实现简单的 JSON / INI 解析，这里先返回默认配置
    CHWELL_LOG_DEBUG("Config load_from_file: " << path << " (stub, using defaults)");
    return true;
}

} // namespace core
} // namespace chwell

