#include "chwell/core/config.h"
#include "chwell/core/logger.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cctype>
#include <vector>

namespace chwell {
namespace core {

namespace {

inline std::string trim(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() &&
           std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

} // anonymous namespace

bool Config::load_from_file(const std::string& path) {
    std::vector<std::string> paths;
    paths.push_back(path);
    return load_from_files(paths);
}

bool Config::load_from_files(const std::vector<std::string>& paths) {
    // 重置 KV，但保留构造时设置的默认字段值
    kv_.clear();

    for (const auto& p : paths) {
        if (p.empty()) continue;
        std::ifstream in(p.c_str());
        if (!in.good()) {
            CHWELL_LOG_DEBUG("Config: file not found, skip: " << p);
            continue;
        }
        CHWELL_LOG_INFO("Config: loading file: " << p);
        std::string line;
        while (std::getline(in, line)) {
            std::string t = trim(line);
            if (t.empty()) continue;
            if (t[0] == '#' || t.rfind("//", 0) == 0) continue;

            std::size_t pos = t.find('=');
            if (pos == std::string::npos) {
                pos = t.find(':');
            }

            std::string key;
            std::string value;

            if (pos == std::string::npos) {
                std::istringstream iss(t);
                if (!(iss >> key >> value)) {
                    continue;
                }
            } else {
                key = trim(t.substr(0, pos));
                value = trim(t.substr(pos + 1));
            }

            if (!key.empty()) {
                kv_[key] = value;
            }
        }
    }

    // 根据 KV 更新内部字段
    apply_kv_to_fields();
    // 环境变量最终覆盖
    apply_env_overrides();

    CHWELL_LOG_INFO("Config: effective listen_port=" << listen_port_
                    << ", worker_threads=" << worker_threads_);
    return true;
}

std::string Config::get_string(const std::string& key,
                               const std::string& default_value) const {
    auto it = kv_.find(key);
    if (it != kv_.end()) {
        return it->second;
    }
    return default_value;
}

int Config::get_int(const std::string& key, int default_value) const {
    auto it = kv_.find(key);
    if (it == kv_.end()) {
        return default_value;
    }
    try {
        return std::stoi(it->second);
    } catch (...) {
        return default_value;
    }
}

void Config::set(const std::string& key, const std::string& value) {
    if (key.empty()) return;
    kv_[key] = value;
    apply_kv_to_fields();
}

void Config::apply_kv_to_fields() {
    listen_port_ = get_int("listen_port", listen_port_);
    worker_threads_ = get_int("worker_threads", worker_threads_);
}

void Config::apply_env_overrides() {
    if (const char* env = std::getenv("CHWELL_LISTEN_PORT")) {
        try {
            int v = std::stoi(env);
            if (v > 0 && v <= 65535) {
                listen_port_ = v;
            }
        } catch (...) {
        }
    }

    if (const char* env = std::getenv("CHWELL_WORKER_THREADS")) {
        try {
            int v = std::stoi(env);
            if (v > 0) {
                worker_threads_ = v;
            }
        } catch (...) {
        }
    }
}

} // namespace core
} // namespace chwell

