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
    loaded_files_ = paths;

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

    CHWELL_LOG_INFO("Config: server_name=" << server_name_unlocked()
                    << ", bus_id=" << bus_id_unlocked()
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


// ========== JSON / 环境 / 热加载 / 回滚 / 校验 ==========

std::string Config::detect_format(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "conf";
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return (ext == "json") ? "json" : "conf";
}

// 极简扁平 JSON 解析：支持 {"k":"v","n":1,"b":true,"o":{"x":1}} → k=v, n=1, b=true, o.x=1
// 不支持数组（数组值序列化为无意义文本，仅保证不崩）
bool Config::parse_json_text(const std::string& text) {
    // 去掉注释与换行，按对象递归展开
    std::string s;
    s.reserve(text.size());
    for (char c : text) {
        if (c == '\n' || c == '\r' || c == '\t') s += ' ';
        else s += c;
    }

    std::function<bool(const std::string&, size_t, size_t, const std::string&)> parse_obj;
    parse_obj = [&](const std::string& src, size_t l, size_t r, const std::string& prefix) -> bool {
        // l 指向 '{'，r 指向匹配 '}'
        size_t i = l + 1;
        while (i < r) {
            // skip ws
            while (i < r && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
            if (i >= r) break;
            if (src[i] != '"') return false;
            size_t ke = src.find('"', i + 1);
            if (ke == std::string::npos || ke >= r) return false;
            std::string key = src.substr(i + 1, ke - i - 1);
            i = ke + 1;
            while (i < r && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
            if (i >= r || src[i] != ':') return false;
            ++i;
            while (i < r && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
            if (i >= r) return false;

            std::string full = prefix.empty() ? key : prefix + "." + key;

            if (src[i] == '{') {
                // 找匹配右括号
                int depth = 0;
                size_t j = i;
                for (; j < r; ++j) {
                    if (src[j] == '{') ++depth;
                    else if (src[j] == '}') { --depth; if (depth == 0) break; }
                }
                if (j >= r) return false;
                if (!parse_obj(src, i, j, full)) return false;
                i = j + 1;
            } else if (src[i] == '"') {
                size_t ve = src.find('"', i + 1);
                if (ve == std::string::npos || ve >= r) return false;
                kv_[full] = src.substr(i + 1, ve - i - 1);
                i = ve + 1;
            } else {
                // number / bool / null：读到 , 或 }
                size_t j = i;
                while (j < r && src[j] != ',' && src[j] != '}') ++j;
                std::string raw = src.substr(i, j - i);
                // 去尾空白
                while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.pop_back();
                if (raw == "true") kv_[full] = "1";
                else if (raw == "false") kv_[full] = "0";
                else if (raw == "null") { /* skip */ }
                else kv_[full] = raw;
                i = j;
            }

            while (i < r && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
            if (i < r && src[i] == ',') ++i;
        }
        return true;
    };

    // 去掉外层空白，找第一个 {
    size_t l = s.find('{');
    size_t r = s.rfind('}');
    if (l == std::string::npos || r == std::string::npos || r <= l) return false;
    return parse_obj(s, l, r, "");
}

bool Config::load_json_from_file(const std::string& path) {
    return load_json_from_files({path});
}

bool Config::load_json_from_files(const std::vector<std::string>& paths) {
    std::unique_lock lock(mutex_);
    loaded_files_ = paths;
    bool ok = false;
    for (const auto& p : paths) {
        std::ifstream in(p.c_str());
        if (!in.good()) continue;
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        if (parse_json_text(content)) {
            ok = true;
        } else {
            CHWELL_LOG_ERROR("Config: JSON parse failed for " + p);
        }
    }
    if (ok) {
        apply_kv_to_fields();
        parse_components();
        apply_env_overrides();
    }
    return ok;
}

bool Config::load_for_env(const std::string& dir, const std::string& env) {
    std::vector<std::string> files;
    // 尝试 .conf / .json 各两层
    std::string sep = "/";
    if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) sep = "";
    std::string base = dir.empty() ? std::string() : dir + sep;
    for (const char* ext : {"conf", "json"}) {
        std::string d = base + "default." + ext;
        std::string e = base + env + "." + ext;
        std::ifstream f1(d.c_str());
        if (f1.good()) { files.push_back(d); f1.close(); }
        std::ifstream f2(e.c_str());
        if (f2.good()) { files.push_back(e); f2.close(); }
    }
    if (files.empty()) {
        CHWELL_LOG_WARN("Config: no config files found in " + dir + " for env=" + env);
        return false;
    }
    // 按格式分组加载
    bool ok = true;
    std::vector<std::string> jsons, confs;
    for (auto& f : files) {
        if (detect_format(f) == "json") jsons.push_back(f);
        else confs.push_back(f);
    }
    if (!confs.empty()) ok = load_from_files(confs) && ok;
    if (!jsons.empty()) ok = load_json_from_files(jsons) && ok;
    // 合并记录所有已加载文件
    {
        std::unique_lock lock(mutex_);
        loaded_files_ = files;
    }
    CHWELL_LOG_INFO("Config: loaded env=" + env + " files=" + std::to_string(files.size()));
    return ok;
}

bool Config::reload() {
    std::vector<std::string> files;
    {
        std::shared_lock lock(mutex_);
        files = loaded_files_;
    }
    if (files.empty()) {
        CHWELL_LOG_WARN("Config: reload() called with no loaded files");
        return false;
    }
    bool ok = true;
    std::vector<std::string> jsons, confs;
    for (auto& f : files) {
        if (detect_format(f) == "json") jsons.push_back(f);
        else confs.push_back(f);
    }
    if (!confs.empty()) ok = load_from_files(confs) && ok;
    if (!jsons.empty()) ok = load_json_from_files(jsons) && ok;
    {
        std::unique_lock lock(mutex_);
        loaded_files_ = files;
    }
    if (ok) {
        CHWELL_LOG_INFO("Config: reload ok (" + std::to_string(files.size()) + " files)");
        notify_change();
    }
    return ok;
}

bool Config::check_reload() {
    static std::unordered_map<std::string, std::int64_t> last_mtime;
    std::vector<std::string> files;
    {
        std::shared_lock lock(mutex_);
        files = loaded_files_;
    }
    if (files.empty()) return false;

    bool changed = false;
    for (auto& f : files) {
        struct stat st;
        if (::stat(f.c_str(), &st) != 0) continue;
        auto mt = static_cast<std::int64_t>(st.st_mtime);
        auto it = last_mtime.find(f);
        if (it == last_mtime.end()) {
            last_mtime[f] = mt;
        } else if (it->second != mt) {
            it->second = mt;
            changed = true;
        }
    }
    if (changed) {
        CHWELL_LOG_INFO("Config: file change detected, reloading");
        return reload();
    }
    return false;
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
    return loaded_files_;
}

} // namespace core
} // namespace chwell
