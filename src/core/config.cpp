#include "chwell/core/config.h"

namespace chwell {
namespace core {

bool Config::load_from_file(const std::string& /*path*/) {
    // TODO: 实现简单的 JSON / INI 解析，这里先返回默认配置
    return true;
}

} // namespace core
} // namespace chwell

