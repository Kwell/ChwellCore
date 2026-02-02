#include "chwell/http/http_response.h"

namespace chwell {
namespace http {

std::vector<char> HttpResponse::to_bytes() const {
    std::string data;
    data.reserve(128 + body.size());

    data += "HTTP/1.1 ";
    data += std::to_string(status_code);
    data += " ";
    data += reason;
    data += "\r\n";

    std::map<std::string, std::string>::const_iterator it;
    for (it = headers.begin(); it != headers.end(); ++it) {
        data += it->first;
        data += ": ";
        data += it->second;
        data += "\r\n";
    }

    data += "Content-Length: ";
    data += std::to_string(static_cast<unsigned long long>(body.size()));
    data += "\r\n";

    data += "\r\n";
    data += body;

    return std::vector<char>(data.begin(), data.end());
}

} // namespace http
} // namespace chwell

