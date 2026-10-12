// 热加载测试插件：编译为 SHARED 库，供 test_hot_reload 加载
//
// 导出 ABI：
//   chwell_create_plugin / chwell_destroy_plugin

#include <string>
#include <stdexcept>
#include "chwell/service/plugin.h"
#include "chwell/service/service.h"

namespace {

class HotComponent : public chwell::service::Component {
public:
    std::string name() const override { return "hot-owned"; }
    void on_register(chwell::service::Service& service) override {
        service.track_registration(*this, chwell::core::Registration([this] { cancelled_ = true; }));
    }
    bool PreShut() override { return cancelled_; }
private:
    bool cancelled_ = false;
};

class TestHotPlugin : public chwell::service::IPlugin {
public:
    const std::string& GetName() const override {
        static std::string n = "TestHotPlugin";
        return n;
    }
    int GetVersion() const override { return 2; }
    bool Install(chwell::service::Service& service) override {
        if (!service.add_component<HotComponent>()) return false;
        if (service.get_component("throw-hot-install")) throw std::runtime_error("partial hot install");
        if (service.get_component("reject-hot-install")) return false;
        installed = true;
        return true;
    }
    bool Uninstall(chwell::service::Service& service) override {
        auto* component = service.get_component("hot-owned");
        if (component && !component->PreShut()) return false;
        installed = false;
        return true;
    }

    bool installed = false;
};

} // namespace

#if defined(_WIN32)
#define TEST_PLUGIN_EXPORT __declspec(dllexport)
#else
#define TEST_PLUGIN_EXPORT
#endif
extern "C" TEST_PLUGIN_EXPORT chwell::service::IPlugin* chwell_create_plugin() {
    return new TestHotPlugin();
}

extern "C" TEST_PLUGIN_EXPORT void chwell_destroy_plugin(chwell::service::IPlugin* p) {
    delete p;
}
