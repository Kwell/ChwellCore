#include "chwell/core/config.h"
#include "chwell/core/logger.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cctype>
#include <vector>
#include <shared_mutex>
#include <sys/stat.h>
#include <functional>
#include <iterator>
#include <mutex>

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
    std::vector<ConfigFile> files;
    for (const auto& path : paths) files.push_back({path, false});
    return load_files_unlocked(files, true);
}

bool Config::load_files_unlocked(const std::vector<ConfigFile>& files, bool reset) {
    if (files.empty()) return false;
    Config pending;
    if (!reset) {
        pending.kv_ = kv_;
        pending.loaded_files_ = loaded_files_;
        pending.file_mtimes_ = file_mtimes_;
    }
    for (const auto& file : files) {
        const auto& p = file.path;
        const auto mtime = file_mtime(p);
        std::ifstream in(p.c_str());
        if (!in.good()) {
            CHWELL_LOG_ERROR("Config: cannot read file: " << p);
            return false;
        }
        if (file.json) {
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            if (!pending.parse_json_text(content)) {
                CHWELL_LOG_ERROR("Config: JSON parse failed for " << p);
                return false;
            }
        } else {
            std::string line;
            while (std::getline(in, line)) {
                std::string t = trim(line);
                if (t.empty()) continue;
                if (t[0] == '#' || t.rfind("//", 0) == 0) continue;

                std::size_t pos = t.find('=');
                if (pos == std::string::npos) pos = t.find(':');
                std::string key, value;
                if (pos == std::string::npos) {
                    std::istringstream iss(t);
                    if (!(iss >> key >> value)) continue;
                } else {
                    key = trim(t.substr(0, pos));
                    value = trim(t.substr(pos + 1));
                }
                if (!key.empty()) pending.kv_[key] = remove_quotes(value);
            }
        }
        if (in.bad() || file_mtime(p) != mtime) {
            CHWELL_LOG_ERROR("Config: read failed or file changed while loading: " << p);
            return false;
        }
        pending.loaded_files_.push_back(file);
        pending.file_mtimes_[p] = mtime;
    }
    // 防止后续文件加载期间，先前读取的层发生变化。
    for (const auto& file : files) {
        if (file_mtime(file.path) != pending.file_mtimes_.at(file.path)) return false;
    }
    pending.apply_kv_to_fields();
    pending.parse_components();
    pending.apply_env_overrides();
    kv_.swap(pending.kv_);
    components_.swap(pending.components_);
    loaded_files_.swap(pending.loaded_files_);
    file_mtimes_.swap(pending.file_mtimes_);
    listen_port_ = pending.listen_port_;
    worker_threads_ = pending.worker_threads_;
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
    apply_env_overrides();
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
    listen_port_ = get_int_unlocked("listen_port", 9000);
    worker_threads_ = get_int_unlocked("worker_threads", 4);
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


// ========== JSON / 环境 / 热加载 / 回滚 / 校验 ==========

// 支持对象、字符串、数字、布尔值和 null；嵌套对象展开为点号键。
// 数组不属于配置格式；非法语法和超过 64 层的嵌套返回 false。
bool Config::parse_json_text(const std::string& text) {
    std::size_t pos = 0;
    auto skip_ws = [&] {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
               text[pos] == '\r' || text[pos] == '\n')) ++pos;
    };
    auto consume = [&](char c) {
        if (pos == text.size() || text[pos] != c) return false;
        ++pos;
        return true;
    };
    auto hex4 = [&](std::uint32_t& value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos == text.size()) return false;
            const char c = text[pos++];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return false;
            value = (value << 4) | digit;
        }
        return true;
    };
    auto append_utf8 = [](std::string& out, std::uint32_t value) {
        if (value <= 0x7f) out += static_cast<char>(value);
        else if (value <= 0x7ff) {
            out += static_cast<char>(0xc0 | (value >> 6));
            out += static_cast<char>(0x80 | (value & 0x3f));
        } else if (value <= 0xffff) {
            out += static_cast<char>(0xe0 | (value >> 12));
            out += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (value & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (value >> 18));
            out += static_cast<char>(0x80 | ((value >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (value & 0x3f));
        }
    };
    auto parse_string = [&](std::string& out) {
        if (!consume('"')) return false;
        while (pos < text.size()) {
            char c = text[pos++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (pos == text.size()) return false;
            switch (text[pos++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t value;
                    if (!hex4(value)) return false;
                    if (value >= 0xd800 && value <= 0xdbff) {
                        std::uint32_t low;
                        if (!consume('\\') || !consume('u') || !hex4(low) ||
                            low < 0xdc00 || low > 0xdfff) return false;
                        value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
                    } else if (value >= 0xdc00 && value <= 0xdfff) return false;
                    append_utf8(out, value);
                    break;
                }
                default: return false;
            }
        }
        return false;
    };
    auto digit = [&] { return pos < text.size() && text[pos] >= '0' && text[pos] <= '9'; };
    auto parse_number = [&](std::string& out) {
        const auto start = pos;
        consume('-');
        if (!consume('0')) {
            if (!digit() || text[pos] == '0') return false;
            while (digit()) ++pos;
        }
        if (consume('.')) {
            if (!digit()) return false;
            while (digit()) ++pos;
        }
        if (consume('e') || consume('E')) {
            if (!consume('+')) consume('-');
            if (!digit()) return false;
            while (digit()) ++pos;
        }
        out = text.substr(start, pos - start);
        return true;
    };
    std::function<bool(const std::string&, unsigned)> parse_obj;
    parse_obj = [&](const std::string& prefix, unsigned depth) {
        if (depth > 64 || !consume('{')) return false;
        skip_ws();
        if (consume('}')) return true;
        while (pos < text.size()) {
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (!consume(':')) return false;
            skip_ws();
            const std::string full = prefix.empty() ? key : prefix + "." + key;
            if (pos == text.size()) return false;
            if (text[pos] == '{') {
                if (!parse_obj(full, depth + 1)) return false;
            } else if (text[pos] == '"') {
                std::string value;
                if (!parse_string(value)) return false;
                kv_[full] = std::move(value);
            } else if (text.compare(pos, 4, "true") == 0) {
                kv_[full] = "1";
                pos += 4;
            } else if (text.compare(pos, 5, "false") == 0) {
                kv_[full] = "0";
                pos += 5;
            } else if (text.compare(pos, 4, "null") == 0) {
                pos += 4;  // null 不产生配置项，保持原有语义
            } else {
                std::string value;
                if (!parse_number(value)) return false;
                kv_[full] = std::move(value);
            }
            skip_ws();
            if (consume('}')) return true;
            if (!consume(',')) return false;
            skip_ws();  // 逗号后必须有下一个键，不能直接闭合
        }
        return false;
    };
    skip_ws();
    if (!parse_obj("", 1)) return false;
    skip_ws();
    return pos == text.size();
}

bool Config::load_json_from_file(const std::string& path) {
    return load_json_from_files({path}, /*reset=*/true);
}

bool Config::load_json_from_files(const std::vector<std::string>& paths, bool reset) {
    std::unique_lock lock(mutex_);
    std::vector<ConfigFile> files;
    for (const auto& path : paths) files.push_back({path, true});
    return load_files_unlocked(files, reset);
}

bool Config::load_for_env(const std::string& dir, const std::string& env) {
    std::vector<ConfigFile> files;
    // 先加载默认层的两种格式，再加载环境层的两种格式。
    std::string sep = "/";
    if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) sep = "";
    std::string base = dir.empty() ? std::string() : dir + sep;
    std::vector<std::string> profiles{"default"};
    if (env != "default") profiles.push_back(env);
    for (const auto& profile : profiles) {
        for (const char* ext : {"conf", "json"}) {
            const std::string path = base + profile + "." + ext;
            struct stat st;
            if (::stat(path.c_str(), &st) == 0) {
                files.push_back({path, std::string(ext) == "json"});
            }
        }
    }
    if (files.empty()) {
        CHWELL_LOG_WARN("Config: no config files found in " + dir + " for env=" + env);
        return false;
    }
    std::unique_lock lock(mutex_);
    const bool ok = load_files_unlocked(files, true);
    if (ok) CHWELL_LOG_INFO("Config: loaded env=" + env + " files=" + std::to_string(files.size()));
    return ok;
}

bool Config::reload() {
    std::unique_lock lock(mutex_);
    if (loaded_files_.empty()) {
        CHWELL_LOG_WARN("Config: reload() called with no loaded files");
        return false;
    }
    const bool ok = load_files_unlocked(loaded_files_, true);
    lock.unlock();
    if (ok) {
        CHWELL_LOG_INFO("Config: reload ok");
        notify_change();
    }
    return ok;
}

std::int64_t Config::file_mtime(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
#if defined(_WIN32)
    // MSVC stat 只有秒级精度
    return static_cast<std::int64_t>(st.st_mtime);
#elif defined(__APPLE__)
    return static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL +
           static_cast<std::int64_t>(st.st_mtimespec.tv_nsec);
#else
    // Linux 等：用纳秒，同秒内改写也能检出
    return static_cast<std::int64_t>(st.st_mtime) * 1000000000LL +
           static_cast<std::int64_t>(st.st_mtim.tv_nsec);
#endif
}

bool Config::check_reload() {
    std::unique_lock lock(mutex_);
    bool changed = false;
    for (const auto& file : loaded_files_) {
        const auto it = file_mtimes_.find(file.path);
        if (it == file_mtimes_.end() || it->second != file_mtime(file.path)) {
            changed = true;
            break;
        }
    }
    if (!changed) return false;
    CHWELL_LOG_INFO("Config: file change detected, reloading");
    // 仅成功发布后更新基线，失败时下次检查仍可重试。
    const bool ok = load_files_unlocked(loaded_files_, true);
    lock.unlock();
    if (ok) {
        notify_change();
    }
    return ok;
}

void Config::add_change_listener(std::function<void()> cb) {
    std::unique_lock lock(mutex_);
    if (cb) change_listeners_.push_back(std::move(cb));
}

void Config::notify_change() {
    std::vector<std::function<void()>> listeners;
    {
        std::shared_lock lock(mutex_);
        listeners = change_listeners_;
    }
    for (auto& fn : listeners) {
        try { fn(); } catch (...) {}
    }
}

void Config::snapshot() {
    std::unique_lock lock(mutex_);
    snapshots_.push_back(kv_);
    if (snapshots_.size() > 32) snapshots_.erase(snapshots_.begin());
    CHWELL_LOG_DEBUG("Config: snapshot pushed, depth=" + std::to_string(snapshots_.size()));
}

bool Config::rollback() {
    std::unique_lock lock(mutex_);
    if (snapshots_.empty()) {
        CHWELL_LOG_WARN("Config: rollback() with empty snapshot stack");
        return false;
    }
    kv_ = snapshots_.back();
    snapshots_.pop_back();
    apply_kv_to_fields();
    parse_components();
    apply_env_overrides();
    CHWELL_LOG_INFO("Config: rolled back to previous snapshot");
    return true;
}

bool Config::require_keys(const std::vector<std::string>& keys) const {
    std::shared_lock lock(mutex_);
    bool ok = true;
    for (auto& k : keys) {
        if (kv_.find(k) == kv_.end()) {
            CHWELL_LOG_ERROR("Config: missing required key: " + k);
            ok = false;
        }
    }
    return ok;
}

std::vector<std::string> Config::loaded_files() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> paths;
    for (const auto& file : loaded_files_) paths.push_back(file.path);
    return paths;
}

} // namespace core
} // namespace chwell
