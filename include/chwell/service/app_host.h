#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "chwell/core/config.h"
#include "chwell/service/service.h"

namespace chwell {
namespace service {

struct AppManifest {
    int listen_port = 9000;
    int worker_threads = 4;
    bool use_epoll = false;
    int reactor_threads = 1;
    int logic_workers = 0;
    std::vector<core::ComponentConfig> components;
};

// Startup configuration is a snapshot. Factories validate their own parameters
// and construct components without acquiring resources until Init/on_register.
class AppHost {
public:
    using ComponentFactory = std::function<std::unique_ptr<Component>(const core::ComponentConfig&)>;

    bool register_component_factory(const std::string& name, ComponentFactory factory);
    bool configure(const AppManifest& manifest);
    bool configure(const core::Config& config);
    bool start();
    void stop();
    void update();

    Service* service() const { return service_.get(); }
    const std::string& last_error() const { return last_error_; }

private:
    bool fail(const std::string& error);
    std::unordered_map<std::string, ComponentFactory> factories_;
    std::unique_ptr<Service> service_;
    std::string last_error_;
};

} // namespace service
} // namespace chwell
