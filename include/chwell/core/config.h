#pragma once

#include <string>

namespace chwell {
namespace core {

class Config {
public:
    Config() : listen_port_(9000), worker_threads_(4) {}

    bool load_from_file(const std::string& path);

    int listen_port() const { return listen_port_; }
    int worker_threads() const { return worker_threads_; }

private:
    int listen_port_;
    int worker_threads_;
};

} // namespace core
} // namespace chwell

