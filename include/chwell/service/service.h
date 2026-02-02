#pragma once

#include <vector>
#include <memory>
#include <type_traits>

#include "chwell/core/thread_pool.h"
#include "chwell/core/logger.h"
#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_server.h"
#include "chwell/service/component.h"

namespace chwell {
namespace service {

// Service：代表一个具体的游戏服务进程
class Service {
public:
    Service(unsigned short listen_port, std::size_t worker_threads)
        : server_(io_service_, listen_port),
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

        server_.set_message_callback([this](const net::TcpConnectionPtr& conn,
                                            const std::vector<char>& data) {
            dispatch_message(conn, data);
        });
    }

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    ~Service() {
        stop();
    }

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

    void start() {
        server_.start_accept();

        for (std::size_t i = 0; i < worker_threads_; ++i) {
            thread_pool_.post([this]() {
                io_service_.run();
            });
        }

        core::Logger::instance().info("Service started");
    }

    void stop() {
        server_.stop();
        io_service_.stop();
    }

    net::IoService& io_service() { return io_service_; }
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

    net::IoService io_service_;
    net::TcpServer server_;
    core::ThreadPool thread_pool_;
    std::size_t worker_threads_;
    std::vector<std::unique_ptr<Component>> components_;
};

} // namespace service
} // namespace chwell
