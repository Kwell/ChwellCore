#pragma once

#include <string>
#include <map>
#include <cctype>
#include <cctype>
#include <cctype>
#include <cctype>

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
        std::string k = key;
        for (char& c : k) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::map<std::string, std::string>::const_iterator it = headers.find(k);
        if (it != headers.end()) {
            return it->second;
        }
        return std::string();
    }
};

} // namespace http
} // namespace chwell

