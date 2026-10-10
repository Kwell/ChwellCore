#include "chwell/service/app_host.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <unordered_set>

namespace chwell {
namespace service {

namespace {
int config_integer(const core::Config& config, const std::string& key, int fallback) {
    const auto text = config.get_string(key, std::to_string(fallback));
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("Invalid integer: " + key);
    }
    return value;
}

bool config_boolean(const core::Config& config, const std::string& key, bool fallback) {
    const auto value = config.get_string(key, fallback ? "true" : "false");
    if (value == "true" || value == "1" || value == "yes" || value == "on") return true;
    if (value == "false" || value == "0" || value == "no" || value == "off") return false;
    throw std::invalid_argument("Invalid boolean: " + key);
}
} // namespace

bool AppHost::fail(const std::string& error) {
    last_error_ = error;
    CHWELL_LOG_ERROR("AppHost: " << error);
    return false;
}

bool AppHost::register_component_factory(const std::string& name, ComponentFactory factory) {
    if (name.empty() || !factory) return fail("Factory requires a name and callable");
    if (!factories_.emplace(name, std::move(factory)).second) {
        return fail("Duplicate component factory: " + name);
    }
    last_error_.clear();
    return true;
}

bool AppHost::configure(const core::Config& config) {
    try {
        AppManifest manifest;
        // Validate raw values, then retain Config's existing environment overrides.
        (void)config_integer(config, "listen_port", 9000);
        (void)config_integer(config, "worker_threads", 4);
        manifest.listen_port = config.listen_port();
        manifest.worker_threads = config.worker_threads();
        manifest.use_epoll = config_boolean(config, "use_epoll", false);
        manifest.reactor_threads = config_integer(config, "reactor_threads", 1);
        manifest.logic_workers = config_integer(config, "logic_workers", 0);
        manifest.components = config.components();
        for (auto& component : manifest.components) {
            const auto prefix = "component." + component.name + ".";
            component.enabled = config_boolean(config, prefix + "enabled", true);
            component.priority = config_integer(config, prefix + "priority", 100);
        }
        return configure(manifest);
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}

bool AppHost::configure(const AppManifest& manifest) {
    if (service_ && service_->is_running()) return fail("Stop the service before configuring");
    if (manifest.listen_port < 0 || manifest.listen_port > 65535 ||
        manifest.worker_threads <= 0 || manifest.reactor_threads <= 0 || manifest.logic_workers < 0) {
        return fail("Invalid port or thread count");
    }
    std::unordered_set<std::string> names;
    for (const auto& component : manifest.components) {
        if (component.name.empty() || !names.insert(component.name).second) {
            return fail("Empty or duplicate component name: " + component.name);
        }
        if (component.enabled && !factories_.count(component.name)) {
            return fail("Missing component factory: " + component.name);
        }
    }

    auto configs = manifest.components;
    std::sort(configs.begin(), configs.end(), [](const auto& a, const auto& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.name < b.name;
    });
    try {
        std::vector<std::pair<std::unique_ptr<Component>, int>> pending;
        std::unordered_set<std::string> runtime_names;
        for (const auto& config : configs) {
            if (!config.enabled) continue;
            auto component = factories_.at(config.name)(config);
            if (!component || component->name().empty()) {
                return fail("Factory returned a null or unnamed component: " + config.name);
            }
            if (!runtime_names.insert(component->name()).second) {
                return fail("Duplicate runtime component name: " + component->name());
            }
            pending.emplace_back(std::move(component), config.priority);
        }
        // Construct the serving host only after every manifest entry is validated.
        auto candidate = std::make_unique<Service>(static_cast<unsigned short>(manifest.listen_port),
            static_cast<std::size_t>(manifest.worker_threads), manifest.use_epoll,
            manifest.reactor_threads, manifest.logic_workers);
        const bool listener_ready = manifest.use_epoll
            ? candidate->epoll_server()->is_valid() : candidate->tcp_server()->is_valid();
        if (!listener_ready) return fail("Network listener creation failed");
        for (auto& component : pending) {
            if (!candidate->add_component(std::move(component.first), component.second)) {
                return fail("Component registration failed");
            }
        }
        service_ = std::move(candidate);
        last_error_.clear();
        return true;
    } catch (const std::exception& error) {
        return fail(error.what());
    } catch (...) {
        return fail("Component construction or registration threw");
    }
}

bool AppHost::start() {
    if (!service_) return fail("Configure the application before starting");
    if (!service_->start_checked()) return fail("Service startup failed; see lifecycle log");
    last_error_.clear();
    return true;
}

void AppHost::stop() {
    if (service_) service_->stop();
}

void AppHost::update() {
    if (service_) service_->Update();
}

} // namespace service
} // namespace chwell
