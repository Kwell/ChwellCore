// 热加载测试插件：编译为 SHARED 库，供 test_hot_reload 加载
//
// 导出 ABI：
//   chwell_create_plugin / chwell_destroy_plugin

#include <string>
#include "chwell/service/plugin.h"
#include "chwell/service/service.h"

namespace {

class TestHotPlugin : public chwell::service::IPlugin {
public:
    const std::string& GetName() const override {
        static std::string n = "TestHotPlugin";
        return n;
    }
    int GetVersion() const override { return 2; }
    bool Install(chwell::service::Service&) override {
        installed = true;
        return true;
    }
    bool Uninstall(chwell::service::Service&) override {
        installed = false;
        return true;
    }

    bool installed = false;
};

} // namespace

extern "C" chwell::service::IPlugin* chwell_create_plugin() {
    return new TestHotPlugin();
}

extern "C" void chwell_destroy_plugin(chwell::service::IPlugin* p) {
    delete p;
}
