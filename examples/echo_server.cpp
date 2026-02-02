#include <iostream>

#include "chwell/core/logger.h"
#include "chwell/core/config.h"
#include "chwell/service/service.h"

using namespace chwell;

// 一个简单的 Echo 组件：注册到 Service 后，就让该服务具备 “Echo” 能力
class EchoComponent : public service::Component {
public:
    virtual std::string name() const override {
        return "EchoComponent";
    }

    virtual void on_message(const net::TcpConnectionPtr& conn,
                            const std::vector<char>& data) override {
        std::string msg(data.begin(), data.end());
        core::Logger::instance().info("EchoComponent received: " + msg);
        conn->send(data);
    }
};

int main() {
    core::Logger::instance().info("Starting Echo Service with components...");

    core::Config cfg;
    cfg.load_from_file("server.conf");

    // 创建一个 Service（内部包含 TcpServer + ThreadPool）
    service::Service svc(static_cast<unsigned short>(cfg.listen_port()),
                         static_cast<std::size_t>(cfg.worker_threads()));

    // 注册 Echo 组件：下游可以按类似方式注册各种业务组件
    svc.add_component<EchoComponent>();

    svc.start();

    core::Logger::instance().info("Echo Service running on port " + std::to_string(cfg.listen_port()));

    // 阻塞主线程，简单实现：等待用户输入退出
    std::cout << "Press ENTER to exit..." << std::endl;
    std::string line;
    std::getline(std::cin, line);

    svc.stop();
    return 0;
}

