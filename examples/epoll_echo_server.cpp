#include "chwell/service/service.h"
#include "chwell/service/component.h"
#include "chwell/core/logger.h"
#include <string>
#include <csignal>

using namespace chwell;

// Echo 组件：收到什么回什么
class EchoComponent : public service::Component {
public:
    std::string name() const override { return "EchoComponent"; }

    void on_message(const net::TcpConnectionPtr& conn,
                    std::string_view data) override {
        CHWELL_LOG_INFO("EchoComponent received " << data.size() << " bytes");
        conn->send(data);  // 虚函数调用 → EpollTcpBridge::send → EpollTcpConnection::send
    }

    void on_disconnect(const net::TcpConnectionPtr& conn) override {
        (void)conn;
        CHWELL_LOG_INFO("EchoComponent: client disconnected");
    }
};

static std::atomic<bool> g_running{true};

void signal_handler(int) { g_running = false; }

int main(int argc, char* argv[]) {
    unsigned short port = 9000;
    int reactor_threads = 1;

    if (argc >= 2) port = static_cast<unsigned short>(std::atoi(argv[1]));
    if (argc >= 3) reactor_threads = std::atoi(argv[2]);

    // 开启 DEBUG 日志
    core::Logger::instance().set_level(core::LogLevel::Debug);

    CHWELL_LOG_INFO("=== Epoll Echo Server ===");
    CHWELL_LOG_INFO("Port: " << port << ", Reactor threads: " << reactor_threads);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // use_epoll=true — 使用 epoll Reactor 模式
    service::Service svc(port, 0, true, reactor_threads);
    svc.add_component<EchoComponent>();
    svc.start();

    CHWELL_LOG_INFO("Epoll echo server running. Press Ctrl+C to stop.");

    while (g_running && svc.is_running()) {
        svc.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    svc.stop();
    CHWELL_LOG_INFO("Server shutdown complete.");
    return 0;
}
