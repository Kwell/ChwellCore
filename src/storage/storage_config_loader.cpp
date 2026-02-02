#include "chwell/storage/storage_config_loader.h"
#include "chwell/core/logger.h"

#include <fstream>
#include <sstream>

#if defined(CHWELL_USE_YAML)
#include <yaml-cpp/yaml.h>
#endif

namespace chwell {
namespace storage {

#if defined(CHWELL_USE_YAML)
static bool parse_yaml(const YAML::Node& root, StorageConfig& out_config) {
    if (!root["storage"]) {
        core::Logger::instance().error("StorageConfigLoader: missing 'storage' key");
        return false;
    }

    const YAML::Node& storage = root["storage"];
    if (storage["type"]) {
        out_config.type = storage["type"].as<std::string>("memory");
    }

    if (out_config.type == "mysql" && storage["mysql"]) {
        const YAML::Node& m = storage["mysql"];
        if (m["host"]) out_config.host = m["host"].as<std::string>();
        if (m["port"]) out_config.port = m["port"].as<int>(3306);
        if (m["database"]) out_config.database = m["database"].as<std::string>();
        if (m["user"]) out_config.user = m["user"].as<std::string>();
        if (m["password"]) out_config.password = m["password"].as<std::string>();
        if (m["charset"]) out_config.extra["charset"] = m["charset"].as<std::string>();
        if (m["table"]) out_config.extra["table"] = m["table"].as<std::string>();
    } else if ((out_config.type == "mongodb" || out_config.type == "mongo") &&
               storage["mongodb"]) {
        const YAML::Node& m = storage["mongodb"];
        if (m["uri"]) out_config.extra["uri"] = m["uri"].as<std::string>();
        if (m["database"]) out_config.database = m["database"].as<std::string>();
        if (m["collection"]) out_config.extra["collection"] = m["collection"].as<std::string>();
    }

    return true;
}
#endif

bool StorageConfigLoader::load(const std::string& yaml_path,
                               StorageConfig& out_config) {
#if defined(CHWELL_USE_YAML)
    try {
        YAML::Node root = YAML::LoadFile(yaml_path);
        return parse_yaml(root, out_config);
    } catch (const std::exception& e) {
        core::Logger::instance().error("StorageConfigLoader: " + std::string(e.what()));
        return false;
    }
#else
    (void)yaml_path;
    (void)out_config;
    core::Logger::instance().error(
        "StorageConfigLoader: YAML support not enabled, rebuild with -DCHWELL_USE_YAML=ON");
    return false;
#endif
}

bool StorageConfigLoader::load_from_string(const std::string& yaml_content,
                                            StorageConfig& out_config) {
#if defined(CHWELL_USE_YAML)
    try {
        YAML::Node root = YAML::Load(yaml_content);
        return parse_yaml(root, out_config);
    } catch (const std::exception& e) {
        core::Logger::instance().error("StorageConfigLoader: " + std::string(e.what()));
        return false;
    }
#else
    (void)yaml_content;
    (void)out_config;
    core::Logger::instance().error(
        "StorageConfigLoader: YAML support not enabled");
    return false;
#endif
}

}  // namespace storage
}  // namespace chwell
