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

std::vector<std::string> config_dependencies(const core::Config& config, const std::string& key) {
    const auto text = config.get_string(key, "");
    std::vector<std::string> result;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return result;
    std::size_t start = 0;
    while (true) {
        const auto end = text.find(',', start);
        const auto token = text.substr(start, end == std::string::npos ? end : end - start);
        const auto first = token.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) throw std::invalid_argument("Empty dependency in " + key);
        const auto last = token.find_last_not_of(" \t\r\n");
        result.push_back(token.substr(first, last - first + 1));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
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
            component.dependencies = config_dependencies(config, prefix + "depends_on");
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
        configs.erase(std::remove_if(configs.begin(), configs.end(), [](const auto& config) {
            return !config.enabled;
        }), configs.end());
        std::vector<ComponentSpec> specs;
        for (const auto& config : configs) specs.push_back({config.name, config.priority, config.dependencies});
        std::vector<std::size_t> manifest_order;
        std::string error;
        if (!resolve_component_order(specs, manifest_order, &error)) return fail(error);

        struct PendingComponent {
            std::unique_ptr<Component> component;
            int priority;
            std::vector<std::string> dependencies;
            std::vector<std::string> manifest_dependencies;
        };
        std::vector<PendingComponent> pending;
        std::unordered_map<std::string, std::string> entry_to_runtime;
        std::unordered_set<std::string> runtime_names;
        for (auto index : manifest_order) {
            const auto& config = configs[index];
            auto component = factories_.at(config.name)(config);
            if (!component || component->name().empty()) {
                return fail("Factory returned a null or unnamed component: " + config.name);
            }
            if (!runtime_names.insert(component->name()).second) {
                return fail("Duplicate runtime component name: " + component->name());
            }
            entry_to_runtime.emplace(config.name, component->name());
            auto dependencies = component->dependencies();
            pending.push_back({std::move(component), config.priority, std::move(dependencies), config.dependencies});
        }
        specs.clear();
        for (auto& item : pending) {
            for (auto& entry : item.manifest_dependencies) {
                entry = entry_to_runtime.at(entry);
                item.dependencies.push_back(entry);
            }
            specs.push_back({item.component->name(), item.priority, item.dependencies});
        }
        std::vector<std::size_t> runtime_order;
        if (!resolve_component_order(specs, runtime_order, &error)) return fail(error);
        // Construct the serving host only after every manifest entry is validated.
        auto candidate = std::make_unique<Service>(static_cast<unsigned short>(manifest.listen_port),
            static_cast<std::size_t>(manifest.worker_threads), manifest.use_epoll,
            manifest.reactor_threads, manifest.logic_workers);
        const bool listener_ready = manifest.use_epoll
            ? candidate->epoll_server()->is_valid() : candidate->tcp_server()->is_valid();
        if (!listener_ready) return fail("Network listener creation failed");
        for (auto index : runtime_order) {
            auto& item = pending[index];
            if (!candidate->add_component(std::move(item.component), item.priority, std::move(item.manifest_dependencies))) {
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
    if (!service_->start_checked()) return fail(service_->last_error());
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
