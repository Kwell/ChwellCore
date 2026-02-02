#include "chwell/storage/storage_factory.h"
#include "chwell/storage/storage_config_loader.h"
#include "chwell/storage/memory_storage.h"
#include "chwell/storage/mysql_storage.h"
#include "chwell/storage/mongodb_storage.h"
#include "chwell/core/logger.h"

#include <cstdlib>

namespace chwell {
namespace storage {

std::unique_ptr<StorageInterface> StorageFactory::create(
    const StorageConfig& config) {
    std::string type = config.type;
    if (type.empty()) type = "memory";

    if (type == "memory") {
        return std::unique_ptr<StorageInterface>(new MemoryStorage());
    }
    if (type == "mysql") {
        std::unique_ptr<MysqlStorage> storage(new MysqlStorage(config));
        if (storage->connect()) {
            return std::unique_ptr<StorageInterface>(storage.release());
        }
        core::Logger::instance().error("StorageFactory: MySQL connect failed");
        return nullptr;
    }
    if (type == "mongodb" || type == "mongo") {
        std::unique_ptr<MongodbStorage> storage(new MongodbStorage(config));
        if (storage->connect()) {
            return std::unique_ptr<StorageInterface>(storage.release());
        }
        core::Logger::instance().error("StorageFactory: MongoDB connect failed");
        return nullptr;
    }

    core::Logger::instance().error("StorageFactory: unknown type '" + type + "'");
    return nullptr;
}

std::unique_ptr<StorageInterface> StorageFactory::create(
    const std::string& type, const std::string& host, int port,
    const std::string& database) {
    StorageConfig config;
    config.type = type;
    config.host = host.empty() ? "127.0.0.1" : host;
    config.port = port;
    config.database = database;
    return create(config);
}

std::unique_ptr<StorageInterface> StorageFactory::create_from_yaml(
    const std::string& yaml_path) {
    std::string path = yaml_path;
    if (const char* env = std::getenv("STORAGE_CONFIG")) {
        path = env;
    }
    StorageConfig config;
    if (!StorageConfigLoader::load(path, config)) {
        core::Logger::instance().error("StorageFactory: failed to load config from " + path);
        return nullptr;
    }
    return create(config);
}

}  // namespace storage
}  // namespace chwell
