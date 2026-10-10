#include <gtest/gtest.h>
#include <stdexcept>
#include "chwell/service/app_host.h"

using namespace chwell;

namespace {
class HostComponent : public service::Component {
public:
    HostComponent(std::string name, std::vector<std::string>& events, std::vector<std::string> dependencies = {})
        : name_(std::move(name)), events_(events), dependencies_(std::move(dependencies)) {}
    std::string name() const override { return name_; }
    int priority() const override { return 999; }
    std::vector<std::string> dependencies() const override { return dependencies_; }
    bool Init() override { events_.push_back(name_); return true; }
private:
    std::string name_;
    std::vector<std::string>& events_;
    std::vector<std::string> dependencies_;
};

service::AppHost::ComponentFactory factory(std::vector<std::string>& events) {
    return [&events](const core::ComponentConfig& config) {
        return std::make_unique<HostComponent>(config.name, events);
    };
}
} // namespace

TEST(AppHostDependencyTest, ManifestEntryAliasesResolveToRuntimeNamesAndOverridePriority) {
    std::vector<std::string> events, constructed;
    service::AppHost host;
    ASSERT_TRUE(host.register_component_factory("DatabaseFactory", [&](const auto& config) {
        constructed.push_back(config.name);
        return std::make_unique<HostComponent>("RuntimeDatabase", events);
    }));
    ASSERT_TRUE(host.register_component_factory("PlayersFactory", [&](const auto& config) {
        constructed.push_back(config.name);
        EXPECT_EQ(config.dependencies, (std::vector<std::string>{"DatabaseFactory"}));
        EXPECT_EQ(config.params.at("depends_on"), " DatabaseFactory ");
        return std::make_unique<HostComponent>("RuntimePlayers", events,
                                               std::vector<std::string>{"RuntimeDatabase"});
    }));
    core::Config config;
    config.set("listen_port", "0");
    config.set("worker_threads", "1");
    config.set("component.PlayersFactory.priority", "-100");
    config.set("component.DatabaseFactory.priority", "100");
    config.set("component.PlayersFactory.depends_on", " DatabaseFactory ");
    ASSERT_TRUE(host.configure(config)) << host.last_error();
    EXPECT_EQ(constructed, (std::vector<std::string>{"DatabaseFactory", "PlayersFactory"}));
    ASSERT_TRUE(host.start()) << host.last_error();
    EXPECT_EQ(events, (std::vector<std::string>{"RuntimeDatabase", "RuntimePlayers"}));
    host.stop();
}

TEST(AppHostDependencyTest, InvalidManifestGraphFailsBeforeFactoriesAndPreservesPreparedHost) {
    std::vector<std::string> events;
    int calls = 0;
    service::AppHost host;
    for (const auto* name : {"a", "b"}) {
        ASSERT_TRUE(host.register_component_factory(name, [&](const auto& config) {
            ++calls;
            return std::make_unique<HostComponent>(config.name, events);
        }));
    }
    service::AppManifest manifest;
    manifest.listen_port = 0;
    manifest.worker_threads = 1;
    ASSERT_TRUE(host.configure(manifest));
    auto* original = host.service();
    core::ComponentConfig a, b;
    a.name = "a"; b.name = "b";
    for (const auto& target : {"absent", "a", "b"}) {
        a.dependencies = {target};
        b.enabled = false;
        manifest.components = {a, b};
        EXPECT_FALSE(host.configure(manifest));
        EXPECT_NE(host.last_error().find(std::string("a -> ") + target), std::string::npos);
        EXPECT_EQ(calls, 0);
        EXPECT_EQ(host.service(), original);
    }
    b.enabled = true;
    a.dependencies = {"b"}; b.dependencies = {"a"};
    manifest.components = {a, b};
    EXPECT_FALSE(host.configure(manifest));
    EXPECT_EQ(host.last_error(), "Component dependency cycle: a -> b -> a");
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(host.service(), original);
    EXPECT_TRUE(host.start());
    host.stop();
}

TEST(AppHostDependencyTest, MalformedListsAreRejectedAndMultipleDependenciesAreParsed) {
    std::vector<std::string> events;
    service::AppHost host;
    int calls = 0;
    for (const auto* name : {"a", "b", "c"})
        host.register_component_factory(name, [&](const auto& config) {
            ++calls;
            if (config.name == "a") EXPECT_EQ(config.dependencies, (std::vector<std::string>{"b", "c"}));
            return std::make_unique<HostComponent>(config.name, events);
        });
    core::Config config;
    config.set("listen_port", "0"); config.set("worker_threads", "1");
    config.set("component.a.priority", "0");
    config.set("component.b.priority", "10"); config.set("component.c.priority", "20");
    for (const auto* invalid : {",b", "b,", "b, ,c"}) {
        config.set("component.a.depends_on", invalid);
        EXPECT_FALSE(host.configure(config));
        EXPECT_NE(host.last_error().find("component.a.depends_on"), std::string::npos);
        EXPECT_EQ(calls, 0);
    }
    config.set("component.a.depends_on", "b, c");
    ASSERT_TRUE(host.configure(config));
    ASSERT_TRUE(host.start());
    EXPECT_EQ(events, (std::vector<std::string>{"b", "c", "a"}));
    host.stop();
}

TEST(AppHostDependencyTest, CodeAndManifestDependenciesFormOneGraphBeforeRegistration) {
    std::vector<std::string> events;
    service::AppHost host;
    int registrations = 0;
    class CountRegistration : public HostComponent {
    public:
        CountRegistration(std::string name, std::vector<std::string>& events,
                          std::vector<std::string> dependencies, int& registrations)
            : HostComponent(std::move(name), events, std::move(dependencies)), registrations_(registrations) {}
        void on_register(service::Service&) override { ++registrations_; }
    private:
        int& registrations_;
    };
    for (const auto* name : {"a", "b"})
        host.register_component_factory(name, [&](const auto& config) {
            return std::make_unique<CountRegistration>(config.name, events,
                config.name == "b" ? std::vector<std::string>{"a"} : std::vector<std::string>{}, registrations);
        });
    service::AppManifest manifest;
    manifest.listen_port = 0; manifest.worker_threads = 1;
    ASSERT_TRUE(host.configure(manifest));
    auto* original = host.service();
    core::ComponentConfig a, b;
    a.name = "a"; b.name = "b"; a.dependencies = {"b"};
    manifest.components = {a, b};
    EXPECT_FALSE(host.configure(manifest));
    EXPECT_EQ(host.last_error(), "Component dependency cycle: b -> a -> b");
    EXPECT_EQ(registrations, 0);
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(host.service(), original);
    a.dependencies.clear(); manifest.components = {a, b};
    ASSERT_TRUE(host.configure(manifest));
    EXPECT_EQ(registrations, 2);
    ASSERT_TRUE(host.start());
    EXPECT_EQ(events, (std::vector<std::string>{"a", "b"}));
    host.stop();
}

TEST(AppHostDependencyTest, StartupRevalidatesDeclarationsAndPropagatesTheDiagnostic) {
    std::vector<std::string> events;
    std::vector<std::string> dependencies;
    class DynamicDeclaration : public HostComponent {
    public:
        DynamicDeclaration(std::vector<std::string>& events, const std::vector<std::string>& dependencies)
            : HostComponent("game", events), dependencies_(dependencies) {}
        std::vector<std::string> dependencies() const override { return dependencies_; }
    private:
        const std::vector<std::string>& dependencies_;
    };
    service::AppHost host;
    host.register_component_factory("game", [&](const auto&) {
        return std::make_unique<DynamicDeclaration>(events, dependencies);
    });
    service::AppManifest manifest;
    manifest.listen_port = 0; manifest.worker_threads = 1;
    core::ComponentConfig game; game.name = "game"; manifest.components = {game};
    ASSERT_TRUE(host.configure(manifest));
    dependencies = {"absent"};
    EXPECT_FALSE(host.start());
    EXPECT_EQ(host.last_error(), "Missing dependency: game -> absent");
    EXPECT_TRUE(events.empty());
    dependencies.clear();
    ASSERT_TRUE(host.start());
    host.stop();
}

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
