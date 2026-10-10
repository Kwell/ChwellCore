#include <gtest/gtest.h>
#include <stdexcept>
#include "chwell/service/app_host.h"

using namespace chwell;

namespace {
class HostComponent : public service::Component {
public:
    HostComponent(std::string name, std::vector<std::string>& events)
        : name_(std::move(name)), events_(events) {}
    std::string name() const override { return name_; }
    int priority() const override { return 999; }
    bool Init() override { events_.push_back(name_); return true; }
private:
    std::string name_;
    std::vector<std::string>& events_;
};

service::AppHost::ComponentFactory factory(std::vector<std::string>& events) {
    return [&events](const core::ComponentConfig& config) {
        return std::make_unique<HostComponent>(config.name, events);
    };
}
} // namespace

TEST(AppHostTest, ConfigDrivesEnablementPriorityAndParameters) {
    std::vector<std::string> events;
    service::AppHost host;
    core::Config config;
    config.set("listen_port", "0");
    config.set("worker_threads", "1");
    config.set("component.last.priority", "20");
    config.set("component.first.priority", "10");
    config.set("component.first.limit", "12");
    config.set("component.disabled.enabled", "false");
    ASSERT_TRUE(host.register_component_factory("last", factory(events)));
    ASSERT_TRUE(host.register_component_factory("first", [&events](const auto& component) {
        EXPECT_EQ(component.params.at("limit"), "12");
        return std::make_unique<HostComponent>(component.name, events);
    }));
    ASSERT_TRUE(host.configure(config));
    EXPECT_EQ(host.service()->component_count(), 2u);
    ASSERT_TRUE(host.start());
    EXPECT_EQ(events, (std::vector<std::string>{"first", "last"}));
    EXPECT_FALSE(host.configure(config));
    host.stop();
}

TEST(AppHostTest, ValidatesManifestBeforeInvokingFactoriesAndPreservesPreparedHost) {
    std::vector<std::string> events;
    service::AppHost host;
    int calls = 0;
    ASSERT_TRUE(host.register_component_factory("one", [&events, &calls](const auto& config) {
        ++calls;
        return std::make_unique<HostComponent>(config.name, events);
    }));
    service::AppManifest manifest;
    manifest.listen_port = 0;
    manifest.worker_threads = 1;
    core::ComponentConfig one;
    one.name = "one";
    manifest.components.push_back(one);
    ASSERT_TRUE(host.configure(manifest));
    auto* original = host.service();
    manifest.components.push_back(one);
    EXPECT_FALSE(host.configure(manifest));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(host.service(), original);
    manifest.components.back().name = "missing";
    EXPECT_FALSE(host.configure(manifest));
    EXPECT_EQ(calls, 1);
    manifest.components.pop_back();
    manifest.worker_threads = 0;
    EXPECT_FALSE(host.configure(manifest));
    EXPECT_EQ(host.service(), original);
}

TEST(AppHostTest, RejectsMalformedConfigAndBadFactories) {
    std::vector<std::string> events;
    service::AppHost host;
    EXPECT_FALSE(host.start());
    EXPECT_FALSE(host.register_component_factory("", factory(events)));
    EXPECT_FALSE(host.register_component_factory("null", {}));
    ASSERT_TRUE(host.register_component_factory("one", factory(events)));
    EXPECT_FALSE(host.register_component_factory("one", factory(events)));
    core::Config config;
    config.set("listen_port", "0");
    config.set("worker_threads", "1garbage");
    EXPECT_FALSE(host.configure(config));
    config.set("worker_threads", "1");
    config.set("component.one.priority", "invalid");
    EXPECT_FALSE(host.configure(config));
    config.set("component.one.priority", "1");
    config.set("component.one.enabled", "maybe");
    EXPECT_FALSE(host.configure(config));
    EXPECT_FALSE(host.last_error().empty());
    ASSERT_TRUE(host.register_component_factory("empty", [](const auto&) {
        return std::unique_ptr<service::Component>();
    }));
    service::AppManifest manifest;
    manifest.listen_port = 0;
    manifest.worker_threads = 1;
    core::ComponentConfig empty;
    empty.name = "empty";
    manifest.components.push_back(empty);
    EXPECT_FALSE(host.configure(manifest));
    EXPECT_EQ(host.service(), nullptr);
}

TEST(AppHostTest, FactoryExceptionLeavesExistingHostAvailable) {
    std::vector<std::string> events;
    service::AppHost host;
    service::AppManifest manifest;
    manifest.listen_port = 0;
    manifest.worker_threads = 1;
    ASSERT_TRUE(host.configure(manifest));
    auto* original = host.service();
    ASSERT_TRUE(host.register_component_factory("bad", [](const auto&) -> std::unique_ptr<service::Component> {
        throw std::invalid_argument("Invalid business parameter");
    }));
    core::ComponentConfig bad;
    bad.name = "bad";
    manifest.components.push_back(bad);
    EXPECT_FALSE(host.configure(manifest));
    EXPECT_EQ(host.service(), original);
    EXPECT_EQ(host.last_error(), "Invalid business parameter");
}

TEST(AppHostTest, RejectsDuplicateRuntimeNamesBeforeRegistration) {
    std::vector<std::string> events;
    service::AppHost host;
    auto same_name = [&events](const auto&) {
        return std::make_unique<HostComponent>("same-runtime-name", events);
    };
    ASSERT_TRUE(host.register_component_factory("one", same_name));
    ASSERT_TRUE(host.register_component_factory("two", same_name));
    service::AppManifest manifest;
    manifest.listen_port = 0;
    manifest.worker_threads = 1;
    core::ComponentConfig one, two;
    one.name = "one";
    two.name = "two";
    manifest.components = {one, two};
    EXPECT_FALSE(host.configure(manifest));
    EXPECT_EQ(host.service(), nullptr);
    EXPECT_TRUE(events.empty());
}

TEST(AppHostTest, ListenerFailurePreservesExistingHost) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    struct SocketGuard { int fd; ~SocketGuard() { ::close(fd); } } guard{fd};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(fd, 1), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
    service::AppHost host;
    service::AppManifest manifest;
    manifest.listen_port = 0;
    manifest.worker_threads = 1;
    ASSERT_TRUE(host.configure(manifest));
    auto* original = host.service();
    manifest.listen_port = ntohs(address.sin_port);
    for (bool epoll : {false, true}) {
        manifest.use_epoll = epoll;
        EXPECT_FALSE(host.configure(manifest));
        EXPECT_EQ(host.service(), original);
        EXPECT_EQ(host.last_error(), "Network listener creation failed");
    }
    EXPECT_TRUE(host.start());
    host.stop();
}
