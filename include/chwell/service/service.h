#pragma once

#include <memory>
#include <vector>
#include <string_view>
#include <functional>
#include <unordered_map>
#include <atomic>

#include "chwell/core/thread_pool.h"
#include "chwell/core/logger.h"
#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_server.h"
#include "chwell/net/epoll_server.h"
#include "chwell/net/epoll_bridge.h"
#include "chwell/net/logic_thread.h"
#include "chwell/service/component.h"
#include "chwell/service/plugin.h"

namespace chwell {
namespace service {

/**
 * @brief Service：代表一个具体的游戏服务进程
 *
 * 支持：
 * - 组件的 7 阶段生命周期管理
 * - 插件系统（Plugin → Component）
 * - 两种网络模式：legacy（阻塞 I/O + IoService）和 epoll（事件驱动）
 * - Logic Thread：epoll 模式下保证业务逻辑单线程执行，消除数据竞争
 *
 * 架构（epoll 模式）：
 *   Reactor Thread 0 ──→ 投递到 LogicThread ──┐
 *   Reactor Thread 1 ──→ 投递到 LogicThread ──┼──→ 单线程按序消费
 *   Reactor Thread 2 ──→ 投递到 LogicThread ──┘     ↓
 *                                              Component::on_message()
 *
 * 生命周期：
 * Init → PostInit → CheckConfig → PreUpdate → Update(循环) → PreShut → Shut
 */
class Service {
public:
    Service(unsigned short listen_port, std::size_t worker_threads, bool use_epoll = false,
            int reactor_threads = 1)
        : use_epoll_(use_epoll),
          thread_pool_(worker_threads),
          worker_threads_(worker_threads),
          running_(false),
          init_stage_(0),
          io_service_ptr_(std::make_unique<net::IoService>()),
          io_service_(*io_service_ptr_) {

        if (use_epoll_) {
            // 初始化 Logic Thread（单线程消费业务逻辑）
            logic_thread_ = std::make_unique<net::LogicThread>();
            logic_thread_->set_message_handler(
                [this](const net::TcpConnectionPtr& conn, std::string_view data) {
                    for (auto& comp : components_) comp->on_message(conn, data);
                });
            logic_thread_->set_disconnect_handler(
                [this](const net::TcpConnectionPtr& conn) {
                    for (auto& comp : components_) comp->on_disconnect(conn);
                });

            epoll_server_ = std::make_unique<net::EpollTcpServer>(listen_port, reactor_threads);

            epoll_server_->set_connection_callback([this](const net::EpollTcpConnectionPtr& conn) {
                CHWELL_LOG_INFO("New connection (epoll) fd=" << conn->native_handle());
                // 创建 bridge，将 EpollTcpConnection 适配为 TcpConnectionPtr
                auto bridge = std::make_shared<net::EpollTcpBridge>(conn);
                int fd = conn->native_handle();
                {
                    std::lock_guard<std::mutex> lock(bridge_mutex_);
                    bridge_map_[fd] = bridge;
                }
            });

            epoll_server_->set_disconnect_callback([this](const net::EpollTcpConnectionPtr& conn) {
                CHWELL_LOG_INFO("Connection closed (epoll) fd=" << conn->native_handle());
                int fd = conn->native_handle();
                net::TcpConnectionPtr bridge;
                {
                    std::lock_guard<std::mutex> lock(bridge_mutex_);
                    auto it = bridge_map_.find(fd);
                    if (it != bridge_map_.end()) {
                        bridge = it->second;
                        bridge_map_.erase(it);
                    }
                }
                if (bridge) dispatch_disconnect(bridge);
            });

            epoll_server_->set_message_callback([this](const net::EpollTcpConnectionPtr& conn,
                                                       std::string_view data) {
                net::TcpConnectionPtr bridge;
                {
                    std::lock_guard<std::mutex> lock(bridge_mutex_);
                    auto it = bridge_map_.find(conn->native_handle());
                    if (it != bridge_map_.end()) bridge = it->second;
                }
                if (bridge) dispatch_message(bridge, data);
            });
        } else {
            legacy_server_ = std::make_unique<net::TcpServer>(io_service_, listen_port);

            legacy_server_->set_connection_callback([](const net::TcpConnectionPtr& conn) {
                (void)conn;
                CHWELL_LOG_INFO("New connection");
            });

            legacy_server_->set_disconnect_callback([this](const net::TcpConnectionPtr& conn) {
                CHWELL_LOG_INFO("Connection closed");
                dispatch_disconnect(conn);
            });

            legacy_server_->set_message_callback([this](const net::TcpConnectionPtr& conn,
                                                        std::string_view data) {
                dispatch_message(conn, data);
            });
        }
    }

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    ~Service() {
        stop();
    }

    // ========== 组件管理 ==========

    template <typename T, typename... Args>
    T* add_component(Args&&... args) {
        static_assert(std::is_base_of<Component, T>::value,
                      "T must derive from chwell::service::Component");

        std::unique_ptr<T> comp(new T(std::forward<Args>(args)...));
        T* raw = comp.get();
        components_.push_back(std::move(comp));

        // 兼容旧接口
        raw->on_register(*this);

        CHWELL_LOG_INFO("Component registered: " + raw->name());
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

    // ========== 7 阶段生命周期管理 ==========

    bool Init() {
        CHWELL_LOG_INFO("Service Init: initializing components...");

        std::sort(components_.begin(), components_.end(),
            [](const std::unique_ptr<Component>& a, const std::unique_ptr<Component>& b) {
                return a->priority() < b->priority();
            });

        for (auto& comp : components_) {
            if (!comp->Init()) {
                CHWELL_LOG_ERROR("Component Init failed: " + comp->name());
                return false;
            }
            CHWELL_LOG_INFO("Component Init: " + comp->name());
        }
        return true;
    }

    bool PostInit() {
        CHWELL_LOG_INFO("Service PostInit: establishing dependencies...");
        for (auto& comp : components_) {
            if (!comp->PostInit()) {
                CHWELL_LOG_ERROR("Component PostInit failed: " + comp->name());
                return false;
            }
        }
        return true;
    }

    bool CheckConfig() {
        CHWELL_LOG_INFO("Service CheckConfig: validating configuration...");
        for (auto& comp : components_) {
            if (!comp->CheckConfig()) {
                CHWELL_LOG_ERROR("Component CheckConfig failed: " + comp->name());
                return false;
            }
        }
        return true;
    }

    bool PreUpdate() {
        CHWELL_LOG_INFO("Service PreUpdate: preparing for update loop...");
        for (auto& comp : components_) {
            if (!comp->PreUpdate()) {
                CHWELL_LOG_ERROR("Component PreUpdate failed: " + comp->name());
                return false;
            }
        }
        return true;
    }

    // ========== 启动和停止 ==========

    void start() {
        if (!Init()) { CHWELL_LOG_ERROR("Service Init failed, aborting..."); return; }
        init_stage_ = 1;
        if (!PostInit()) { CHWELL_LOG_ERROR("Service PostInit failed, aborting..."); Shut(); return; }
        init_stage_ = 2;
        if (!CheckConfig()) { CHWELL_LOG_ERROR("Service CheckConfig failed, aborting..."); Shut(); return; }
        init_stage_ = 3;
        if (!PreUpdate()) { CHWELL_LOG_ERROR("Service PreUpdate failed, aborting..."); Shut(); return; }
        init_stage_ = 4;

        // 启动 Logic Thread（epoll 模式）
        if (use_epoll_ && logic_thread_) {
            logic_thread_->start();
            CHWELL_LOG_INFO("Service: LogicThread started");
        }

        // 启动网络
        if (use_epoll_) {
            epoll_server_->start();
        } else {
            legacy_server_->start_accept();
            for (std::size_t i = 0; i < worker_threads_; ++i) {
                thread_pool_.post([this]() { io_service_.run(); });
            }
        }

        running_ = true;
        last_update_time_ = std::chrono::steady_clock::now();
        CHWELL_LOG_INFO("Service started successfully"
                        << (use_epoll_ ? " (epoll mode)" : " (legacy mode)"));
    }

    void stop() {
        if (!running_.exchange(false)) return;

        CHWELL_LOG_INFO("Service stopping...");

        // 先停止 Logic Thread，确保排空队列
        if (logic_thread_) {
            logic_thread_->stop();
        }

        if (use_epoll_) {
            if (epoll_server_) epoll_server_->stop();
        } else {
            if (legacy_server_) legacy_server_->stop();
            io_service_.stop();
        }

        PreShut();
        Shut();
        plugin_manager_.UninstallAll(*this);

        {
            std::lock_guard<std::mutex> lock(bridge_mutex_);
            bridge_map_.clear();
        }

        init_stage_ = 0;
        CHWELL_LOG_INFO("Service stopped");
    }

    void PreShut() {
        CHWELL_LOG_INFO("Service PreShut: notifying components...");
        for (auto& comp : components_) comp->PreShut();
    }

    void Shut() {
        CHWELL_LOG_INFO("Service Shut: releasing resources...");
        for (auto& comp : components_) comp->Shut();
    }

    void Update() {
        if (!running_) return;

        auto now = std::chrono::steady_clock::now();
        auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_update_time_).count();
        last_update_time_ = now;

        for (auto& comp : components_) comp->Update(delta_ms);
    }

    // ========== 访问器 ==========

    net::IoService& io_service() { return io_service_; }
    net::TcpServer* tcp_server() { return legacy_server_.get(); }
    net::EpollTcpServer* epoll_server() { return epoll_server_.get(); }
    net::LogicThread* logic_thread() { return logic_thread_.get(); }
    bool is_running() const { return running_; }
    bool use_epoll() const { return use_epoll_; }
    PluginManager& plugin_manager() { return plugin_manager_; }

private:
    /**
     * @brief 分发消息到 Component
     *
     * epoll 模式：投递到 LogicThread，保证单线程消费
     * legacy 模式：直接调用（legacy 本身是单线程）
     */
    void dispatch_message(const net::TcpConnectionPtr& conn,
                          std::string_view data) {
        if (logic_thread_) {
            // epoll 模式：投递到 Logic Thread，保证单线程消费
            logic_thread_->post(conn, data);
        } else {
            // legacy 模式：直接调用
            for (auto& comp : components_) comp->on_message(conn, data);
        }
    }

    /**
     * @brief 分发断连事件到 Component
     */
    void dispatch_disconnect(const net::TcpConnectionPtr& conn) {
        if (logic_thread_) {
            logic_thread_->post_disconnect(conn);
        } else {
            for (auto& comp : components_) comp->on_disconnect(conn);
        }
    }

    bool use_epoll_;
    net::IoService& io_service_;
    std::unique_ptr<net::IoService> io_service_ptr_;
    std::unique_ptr<net::TcpServer> legacy_server_;
    std::unique_ptr<net::EpollTcpServer> epoll_server_;

    // Logic Thread：epoll 模式下的单线程逻辑消费
    std::unique_ptr<net::LogicThread> logic_thread_;

    // epoll 模式下：fd → bridge 的映射
    std::mutex bridge_mutex_;
    std::unordered_map<int, net::TcpConnectionPtr> bridge_map_;

    core::ThreadPool thread_pool_;
    std::size_t worker_threads_;
    std::vector<std::unique_ptr<Component>> components_;

    PluginManager plugin_manager_;
    std::atomic<bool> running_;
    int init_stage_;
    std::chrono::steady_clock::time_point last_update_time_;
};

} // namespace service
} // namespace chwell
