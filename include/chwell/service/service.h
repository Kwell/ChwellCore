#pragma once

#include <vector>
#include <memory>
#include <type_traits>
#include <asio.hpp>

#include "chwell/core/thread_pool.h"
#include "chwell/core/logger.h"
#include "chwell/net/tcp_server.h"
#include "chwell/service/component.h"

namespace chwell {
namespace service {

// Service：代表一个具体的游戏服务进程（例如：网关服、逻辑服、房间服等）
// - 内部持有 TcpServer + ThreadPool
// - 对外暴露组件注册接口，下游只需实现 Component 并注册即可获得功能
class Service {
public:
    Service(unsigned short listen_port, std::size_t worker_threads)
        : io_service_(),
          server_(io_service_, listen_port),
          thread_pool_(worker_threads),
          worker_threads_(worker_threads) {
        using core::Logger;

        server_.set_connection_callback([](const net::TcpConnectionPtr& conn) {
            (void)conn;
            Logger::instance().info("New connection");
        });

        server_.set_disconnect_callback([this](const net::TcpConnectionPtr& conn) {
            Logger::instance().info("Connection closed");
            dispatch_disconnect(conn);
        });

        // 所有网络消息统一先进入 Service，再分发给各个组件
        server_.set_message_callback([this](const net::TcpConnectionPtr& conn,
                                            const std::vector<char>& data) {
            dispatch_message(conn, data);
        });
    }

    // 禁止拷贝
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    ~Service() {
        stop();
    }

    // 注册组件（模板方式，方便下游直接传组件类型和构造参数）
    template <typename T, typename... Args>
    T* add_component(Args&&... args) {
        static_assert(std::is_base_of<Component, T>::value,
                      "T must derive from chwell::service::Component");

        std::unique_ptr<T> comp(new T(std::forward<Args>(args)...));
        T* raw = comp.get();
        components_.push_back(std::move(comp));

        raw->on_register(*this);

        core::Logger::instance().info("Component registered: " + raw->name());
        return raw;
    }

    // 按类型获取组件（如果存在多个同类型组件，返回第一个）
    template <typename T>
    T* get_component() {
        static_assert(std::is_base_of<Component, T>::value,
                      "T must derive from chwell::service::Component");
        for (std::size_t i = 0; i < components_.size(); ++i) {
            T* ptr = dynamic_cast<T*>(components_[i].get());
            if (ptr != 0) {
                return ptr;
            }
        }
        return 0;
    }

    // 启动服务：开始接受连接，并用线程池驱动 io_service
    void start() {
        server_.start_accept();

        for (std::size_t i = 0; i < worker_threads_; ++i) {
            thread_pool_.post([this]() {
                io_service_.run();
            });
        }

        core::Logger::instance().info("Service started");
    }

    // 停止服务（可以从主线程在退出前调用）
    void stop() {
        io_service_.stop();
    }

    asio::io_service& io_service() { return io_service_; }
    net::TcpServer& tcp_server() { return server_; }

private:
    void dispatch_message(const net::TcpConnectionPtr& conn,
                          const std::vector<char>& data) {
        for (std::size_t i = 0; i < components_.size(); ++i) {
            components_[i]->on_message(conn, data);
        }
    }

    void dispatch_disconnect(const net::TcpConnectionPtr& conn) {
        for (std::size_t i = 0; i < components_.size(); ++i) {
            components_[i]->on_disconnect(conn);
        }
    }

    asio::io_service io_service_;
    net::TcpServer server_;
    core::ThreadPool thread_pool_;
    std::size_t worker_threads_;
    std::vector<std::unique_ptr<Component> > components_;
};

} // namespace service
} // namespace chwell

