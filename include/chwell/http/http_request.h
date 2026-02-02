#pragma once

#include <string>
#include <map>

namespace chwell {
namespace http {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;

    // 获取某个 header（不存在则返回空字符串）
    std::string header(const std::string& key) const {
        std::map<std::string, std::string>::const_iterator it = headers.find(key);
        if (it != headers.end()) {
            return it->second;
        }
        return std::string();
    }
};

} // namespace http
} // namespace chwell

