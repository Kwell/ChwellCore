#pragma once

#include <string>
#include <map>
#include <cstdint>

namespace chwell {
namespace storage {
namespace orm {

// Document：ORM 文档/行，类型安全的字段容器
// 序列化为 YAML 存储，上层通过 get/set 访问字段，无需关心底层介质
class Document {
public:
    Document() = default;

    // 类型安全的 getter
    std::string get_string(const std::string& key,
                           const std::string& default_val = "") const;
    std::int64_t get_int64(const std::string& key, std::int64_t default_val = 0) const;
    int get_int(const std::string& key, int default_val = 0) const;
    double get_double(const std::string& key, double default_val = 0) const;
    bool get_bool(const std::string& key, bool default_val = false) const;

    // 类型安全的 setter
    void set_string(const std::string& key, const std::string& value);
    void set_int64(const std::string& key, std::int64_t value);
    void set_int(const std::string& key, int value);
    void set_double(const std::string& key, double value);
    void set_bool(const std::string& key, bool value);

    // 序列化：用于存储
    std::string to_string() const;
    bool from_string(const std::string& data);

    // 原始访问
    bool has(const std::string& key) const;
    void clear() { data_.clear(); }
    const std::map<std::string, std::string>& data() const { return data_; }

private:
    std::map<std::string, std::string> data_;
};

}  // namespace orm
}  // namespace storage
}  // namespace chwell
