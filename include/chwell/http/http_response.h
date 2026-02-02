#pragma once

#include <string>
#include <map>
#include <vector>

namespace chwell {
namespace http {

struct HttpResponse {
    int status_code;
    std::string reason;
    std::map<std::string, std::string> headers;
    std::string body;

    HttpResponse()
        : status_code(200), reason("OK") {}

    void set_header(const std::string& key, const std::string& value) {
        headers[key] = value;
    }

    // 简单序列化为 HTTP 响应报文
    std::vector<char> to_bytes() const;
};

} // namespace http
} // namespace chwell

