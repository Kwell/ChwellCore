#include "chwell/core/config.h"
#include "chwell/core/logger.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cctype>
#include <vector>
#include <shared_mutex>

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

inline std::string remove_quotes(const std::string& s) {
    if (s.size() >= 2) {
        if ((s[0] == '"' && s[s.size()-1] == '"') ||
            (s[0] == '\'' && s[s.size()-1] == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

} // anonymous namespace

bool Config::load_from_file(const std::string& path) {
    std::vector<std::string> paths;
    paths.push_back(path);
    return load_from_files(paths);
}

bool Config::load_from_files(const std::vector<std::string>& paths) {
    std::unique_lock lock(mutex_);

    // 重置 KV，但保留构造时设置的默认字段值
    kv_.clear();
    components_.clear();

    bool any_loaded = false;

    for (const auto& p : paths) {
        if (p.empty()) continue;
        std::ifstream in(p.c_str());
        if (!in.good()) {
            CHWELL_LOG_DEBUG("Config: file not found, skip: " << p);
            continue;
        }
        any_loaded = true;
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
                kv_[key] = remove_quotes(value);
            }
        }
    }

    if (!any_loaded) {
        return false;
    }

    // 根据 KV 更新内部字段
    apply_kv_to_fields();
    // 解析组件配置
    parse_components();
    // 环境变量最终覆盖
    apply_env_overrides();

    CHWELL_LOG_INFO("Config: server_name=" << server_name()
                    << ", bus_id=" << bus_id()
                    << ", listen_port=" << listen_port_
                    << ", worker_threads=" << worker_threads_);
    CHWELL_LOG_INFO("Config: loaded " << components_.size() << " component configs");
    return true;
}

std::string Config::get_string(const std::string& key,
                               const std::string& default_value) const {
    std::shared_lock lock(mutex_);
    auto it = kv_.find(key);
    if (it != kv_.end()) {
        return it->second;
    }
    return default_value;
}

int Config::get_int(const std::string& key, int default_value) const {
    std::shared_lock lock(mutex_);
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

bool Config::get_bool(const std::string& key, bool default_value) const {
    std::shared_lock lock(mutex_);
    auto it = kv_.find(key);
    if (it == kv_.end()) {
        return default_value;
    }
    const std::string& v = it->second;
    if (v == "true" || v == "1" || v == "yes" || v == "on") {
        return true;
    }
    if (v == "false" || v == "0" || v == "no" || v == "off") {
        return false;
    }
    return default_value;
}

void Config::set(const std::string& key, const std::string& value) {
    std::unique_lock lock(mutex_);
    if (key.empty()) return;
    kv_[key] = value;
    apply_kv_to_fields();
    parse_components();
}

std::string Config::get_string_unlocked(const std::string& key,
                               const std::string& default_value) const {
    auto it = kv_.find(key);
    if (it != kv_.end()) {
        return it->second;
    }
    return default_value;
}

int Config::get_int_unlocked(const std::string& key, int default_value) const {
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

bool Config::get_bool_unlocked(const std::string& key, bool default_value) const {
    auto it = kv_.find(key);
    if (it == kv_.end()) {
        return default_value;
    }
    const std::string& v = it->second;
    if (v == "true" || v == "1" || v == "yes" || v == "on") {
        return true;
    }
    if (v == "false" || v == "0" || v == "no" || v == "off") {
        return false;
    }
    return default_value;
}

void Config::apply_kv_to_fields() {
    listen_port_ = get_int_unlocked("listen_port", listen_port_);
    worker_threads_ = get_int_unlocked("worker_threads", worker_threads_);
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

void Config::parse_components() {
    // 解析组件配置，格式：
    // component.name = "ComponentName"
    // component.name.enabled = true
    // component.name.priority = 10
    // component.name.param_key = param_value
    
    std::unordered_map<std::string, ComponentConfig> comp_map;
    
    for (const auto& kv : kv_) {
        if (kv.first.find("component.") == 0) {
            // 解析 component.xxx.yyy
            std::string rest = kv.first.substr(10); // 去掉 "component."
            std::size_t dot_pos = rest.find('.');
            
            if (dot_pos != std::string::npos) {
                std::string comp_name = rest.substr(0, dot_pos);
                std::string prop = rest.substr(dot_pos + 1);
                
                if (prop == "enabled") {
                    comp_map[comp_name].name = comp_name;
                    comp_map[comp_name].enabled = get_bool_unlocked(kv.first, true);
                } else if (prop == "priority") {
                    comp_map[comp_name].name = comp_name;
                    comp_map[comp_name].priority = get_int_unlocked(kv.first, 100);
                } else {
                    // 组件参数
                    comp_map[comp_name].name = comp_name;
                    comp_map[comp_name].params[prop] = kv.second;
                }
            }
        }
    }
    
    // 转换为 vector
    components_.clear();
    for (const auto& kv : comp_map) {
        if (kv.second.name.empty()) continue;
        components_.push_back(kv.second);
    }
    
    CHWELL_LOG_INFO("Config: parsed " << components_.size() << " component configs");
}

bool Config::is_component_enabled(const std::string& name) const {
    std::shared_lock lock(mutex_);
    for (const auto& comp : components_) {
        if (comp.name == name) {
            return comp.enabled;
        }
    }
    // 默认启用
    return true;
}

int Config::get_component_priority(const std::string& name) const {
    std::shared_lock lock(mutex_);
    for (const auto& comp : components_) {
        if (comp.name == name) {
            return comp.priority;
        }
    }
    // 默认优先级
    return 100;
}

} // namespace core
} // namespace chwell
