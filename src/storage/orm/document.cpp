#include "chwell/storage/orm/document.h"

#include <sstream>
#include <cstdlib>
#include <algorithm>

#if defined(CHWELL_USE_YAML)
#include <yaml-cpp/yaml.h>
#endif

namespace chwell {
namespace storage {
namespace orm {

std::string Document::get_string(const std::string& key,
                                  const std::string& default_val) const {
    auto it = data_.find(key);
    return it != data_.end() ? it->second : default_val;
}

std::int64_t Document::get_int64(const std::string& key,
                                  std::int64_t default_val) const {
    auto it = data_.find(key);
    if (it == data_.end()) return default_val;
    return static_cast<std::int64_t>(std::strtoll(it->second.c_str(), nullptr, 10));
}

int Document::get_int(const std::string& key, int default_val) const {
    return static_cast<int>(get_int64(key, default_val));
}

double Document::get_double(const std::string& key, double default_val) const {
    auto it = data_.find(key);
    if (it == data_.end()) return default_val;
    return std::strtod(it->second.c_str(), nullptr);
}

bool Document::get_bool(const std::string& key, bool default_val) const {
    auto it = data_.find(key);
    if (it == data_.end()) return default_val;
    const std::string& v = it->second;
    if (v == "1" || v == "true" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "no") return false;
    return default_val;
}

void Document::set_string(const std::string& key, const std::string& value) {
    data_[key] = value;
}

void Document::set_int64(const std::string& key, std::int64_t value) {
    data_[key] = std::to_string(value);
}

void Document::set_int(const std::string& key, int value) {
    data_[key] = std::to_string(value);
}

void Document::set_double(const std::string& key, double value) {
    data_[key] = std::to_string(value);
}

void Document::set_bool(const std::string& key, bool value) {
    data_[key] = value ? "1" : "0";
}

bool Document::has(const std::string& key) const {
    return data_.find(key) != data_.end();
}

#if defined(CHWELL_USE_YAML)
std::string Document::to_string() const {
    YAML::Emitter out;
    out << YAML::BeginMap;
    for (const auto& p : data_) {
        out << YAML::Key << p.first << YAML::Value << p.second;
    }
    out << YAML::EndMap;
    return std::string(out.c_str());
}

bool Document::from_string(const std::string& data) {
    try {
        YAML::Node node = YAML::Load(data);
        if (!node.IsMap()) return false;
        data_.clear();
        for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
            std::string key = it->first.as<std::string>();
            std::string value = it->second.as<std::string>();
            data_[key] = value;
        }
        return true;
    } catch (...) {
        return false;
    }
}
#else
// 简单格式：key=value\n，value 中 \n 和 = 转义为 \\n 和 \\=
static std::string escape(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '=') r += "\\=";
        else r += static_cast<char>(c);
    }
    return r;
}

static std::string unescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == 'n') r += '\n';
            else if (s[i + 1] == '=') r += '=';
            else if (s[i + 1] == '\\') r += '\\';
            else r += s[i + 1];
            ++i;
        } else {
            r += s[i];
        }
    }
    return r;
}

std::string Document::to_string() const {
    std::ostringstream oss;
    for (const auto& p : data_) {
        oss << escape(p.first) << "=" << escape(p.second) << "\n";
    }
    return oss.str();
}

bool Document::from_string(const std::string& data) {
    data_.clear();
    std::string key, value;
    std::string cur;
    bool in_key = true;
    for (size_t i = 0; i <= data.size(); ++i) {
        char c = (i < data.size()) ? data[i] : '\n';
        if (c == '\n') {
            if (in_key == false && !key.empty()) {
                data_[unescape(key)] = unescape(cur);
            }
            key.clear();
            cur.clear();
            in_key = true;
        } else if (c == '=' && in_key) {
            key = cur;
            cur.clear();
            in_key = false;
        } else if (c == '\\' && i + 1 < data.size()) {
            cur += data[++i];
        } else {
            cur += c;
        }
    }
    return true;
}
#endif

}  // namespace orm
}  // namespace storage
}  // namespace chwell
