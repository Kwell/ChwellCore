#pragma once

#include <memory>
#include <vector>
#include <string_view>
#include <functional>
#include <unordered_map>
#include <atomic>
#include <algorithm>
#include <exception>
#include <optional>

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
            int reactor_threads = 1, int logic_workers = 0)
        : use_epoll_(use_epoll),
          thread_pool_(worker_threads),
          worker_threads_(worker_threads),
          running_(false),
          io_service_ptr_(std::make_unique<net::IoService>()),
          io_service_(*io_service_ptr_) {

        if (use_epoll_) {
            // LogicThreadPool：按 conn_id 分片，多 worker 并行消费业务逻辑
            // logic_workers=0 表示与 reactor_threads 相同（至少 1）
            int lw = logic_workers > 0 ? logic_workers
                    : (reactor_threads > 0 ? reactor_threads : 1);
            logic_pool_ = std::unique_ptr<net::LogicThreadPool>(new net::LogicThreadPool(static_cast<std::size_t>(lw)));
            logic_pool_->set_message_handler(
                [this](const net::TcpConnectionPtr& conn, std::string_view data) {
                    for (auto& comp : components_) comp->on_message(conn, data);
                });
            logic_pool_->set_disconnect_handler(
                [this](const net::TcpConnectionPtr& conn) {
                    for (auto& comp : components_) comp->on_disconnect(conn);
                });

            epoll_server_ = std::make_unique<net::EpollTcpServer>(listen_port, reactor_threads);

            epoll_server_->set_connection_callback([this](const net::EpollTcpConnectionPtr& conn) {
                CHWELL_LOG_INFO("New connection (epoll) fd=" << conn->native_handle());
                // 创建 bridge，将 EpollTcpConnection 适配为 TcpConnectionPtr
                auto bridge = std::make_shared<net::EpollTcpBridge>(conn);
                std::uint64_t cid = conn->conn_id();
                {
                    std::lock_guard<std::mutex> lock(bridge_mutex_);
                    bridge_map_[cid] = bridge;
                }
            });

            epoll_server_->set_disconnect_callback([this](const net::EpollTcpConnectionPtr& conn) {
                CHWELL_LOG_INFO("Connection closed (epoll) fd=" << conn->native_handle());
                std::uint64_t cid = conn->conn_id();
                net::TcpConnectionPtr bridge;
                {
                    std::lock_guard<std::mutex> lock(bridge_mutex_);
                    auto it = bridge_map_.find(cid);
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
                    auto it = bridge_map_.find(conn->conn_id());
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
        registration_blocked_ = true;
        stop();
        std::vector<Component*> remaining;
        for (auto it = components_.rbegin(); it != components_.rend(); ++it) {
            if (component_records_.at(it->get()).needs_shutdown) remaining.push_back(it->get());
        }
        for (auto* component : remaining) {
            cleanup_call(component, true);
            cleanup_call(component, false);
        }
    }

    // ========== 组件管理 ==========

    template <typename T, typename... Args>
    T* add_component(Args&&... args) {
        static_assert(std::is_base_of<Component, T>::value,
                      "T must derive from chwell::service::Component");

        auto comp = std::make_unique<T>(std::forward<Args>(args)...);
        return static_cast<T*>(add_component(std::move(comp)));
    }

    Component* add_component(std::unique_ptr<Component> comp,
                             std::optional<int> priority = std::nullopt) {
        if (registration_blocked_ || running_ || (starting_ && !registration_owner_) ||
            !initialized_components_.empty()) {
            return registration_failed("Component registration requires an inactive service");
        }
        if (!comp) return registration_failed("Component registration rejected: null component");
        Component* raw = comp.get();
        if (raw->name().empty()) {
            return registration_failed("Component registration rejected: empty name");
        }
        for (const auto& existing : components_) {
            if (existing && existing->name() == raw->name()) {
                return registration_failed("Component registration rejected: duplicate name " + raw->name());
            }
        }
        component_records_[raw] = {registration_owner_, priority, true};
        components_.push_back(std::move(comp));

        // 兼容旧接口
        try {
            raw->on_register(*this);
        } catch (...) {
            registration_failed("Component on_register threw: " + raw->name());
            remove_component(raw);
            throw;
        }

        CHWELL_LOG_INFO("Component registered: " + raw->name());
        return raw;
    }

    std::size_t component_count() const { return components_.size(); }
    Component* get_component(const std::string& name) const {
        for (const auto& comp : components_) {
            if (comp->name() == name) return comp.get();
        }
        return nullptr;
    }
    std::string component_owner(const Component* component) const {
        auto it = component_records_.find(component);
        return it != component_records_.end() && it->second.owner
            ? it->second.owner->GetName() : std::string();
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
        if (running_ || !initialized_components_.empty()) return false;
        CHWELL_LOG_INFO("Service Init: initializing components...");

        std::stable_sort(components_.begin(), components_.end(),
            [this](const std::unique_ptr<Component>& a, const std::unique_ptr<Component>& b) {
                const auto& ar = component_records_.at(a.get());
                const auto& br = component_records_.at(b.get());
                return ar.priority.value_or(a->priority()) < br.priority.value_or(b->priority());
            });

        for (auto& comp : components_) {
            // Include failed Init: it may have allocated resources before returning false.
            initialized_components_.push_back(comp.get());
            component_records_.at(comp.get()).needs_shutdown = true;
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
        (void)start_checked();
    }

    bool start_checked() {
        if (running_) return true;
        if (starting_ || stopped_after_run_ || !initialized_components_.empty()) return false;
        starting_ = true;
        try {
            if (!plugin_manager_.InstallAll(*this) || !Init() || !PostInit() ||
                !CheckConfig() || !PreUpdate()) {
                cleanup_start_failure();
                return false;
            }
            if ((use_epoll_ && !epoll_server_->is_valid()) ||
                (!use_epoll_ && !legacy_server_->is_valid())) {
                CHWELL_LOG_ERROR("Service: network listener is not ready");
                cleanup_start_failure();
                return false;
            }

            if (use_epoll_ && logic_pool_) logic_pool_->start();

            network_attempted_ = true;
            const bool network_ready = use_epoll_ ? epoll_server_->start_checked()
                                                  : legacy_server_->start_accept_checked();
            if (!network_ready) {
                cleanup_start_failure();
                return false;
            }
            if (!use_epoll_) {
                for (std::size_t i = 0; i < worker_threads_; ++i) {
                    thread_pool_.post([this]() { io_service_.run(); });
                }
            }

            last_update_time_ = std::chrono::steady_clock::now();
            running_ = true;
            starting_ = false;
            CHWELL_LOG_INFO("Service started successfully"
                            << (use_epoll_ ? " (epoll mode)" : " (legacy mode)"));
            return true;
        } catch (const std::exception& error) {
            CHWELL_LOG_ERROR("Service startup exception: " << error.what());
        } catch (...) {
            CHWELL_LOG_ERROR("Service startup exception");
        }
        cleanup_start_failure();
        return false;
    }

    void stop() {
        if (!running_.exchange(false)) {
            if (!initialized_components_.empty()) cleanup_start_failure();
            else plugin_manager_.UninstallAll(*this);
            return;
        }
        stopped_after_run_ = true;

        CHWELL_LOG_INFO("Service stopping (graceful shutdown)...");

        // ========== 平滑关机流程（P0） ==========

        // Step 1: 停止 Reactor 接收新消息（但保持已连接的客户端读端关闭）
        if (use_epoll_) {
            if (epoll_server_) {
                CHWELL_LOG_INFO("Step 1: Stopping epoll server (no more new messages)");
                epoll_server_->stop();
            }
        } else {
            if (legacy_server_) {
                CHWELL_LOG_INFO("Step 1: Stopping legacy server");
                legacy_server_->stop();
            }
            io_service_.stop();
            thread_pool_.stop();
        }

        // Step 2: 通知组件即将关闭（PreShut），组件应停止接收新请求，标记进入关机状态
        CHWELL_LOG_INFO("Step 2: PreShut - notifying components");
        PreShut();

        // Step 3: 排空 Logic Thread 队列（保证残余消息全部处理完）
        if (logic_pool_) {
            CHWELL_LOG_INFO("Step 3: Draining LogicThreadPool queue (pending="
                            << logic_pool_->pending_count() << ")");
            bool drained = logic_pool_->drain(shutdown_drain_timeout_ms_);
            if (!drained) {
                CHWELL_LOG_WARN("LogicThreadPool drain timeout! pending="
                                << logic_pool_->pending_count() << " messages dropped");
            }
        }

        // Step 4: 执行各组件的 flush 操作（强制脏数据落地）
        CHWELL_LOG_INFO("Step 4: Flushing all dirty data to storage");
        for (auto& comp : components_) {
            try {
                comp->Flush();
            } catch (...) {
                CHWELL_LOG_ERROR("Component Flush threw during shutdown");
            }
        }

        // Step 5: 停止 Logic Thread
        if (logic_pool_) {
            CHWELL_LOG_INFO("Step 5: Stopping LogicThreadPool");
            logic_pool_->stop();
        }

        // Step 6: 停止 IoService（legacy 模式）
        if (!use_epoll_) {
            io_service_.stop();
        }

        // Step 7: 执行 Shut 释放资源
        CHWELL_LOG_INFO("Step 7: Shut - releasing resources");
        Shut();
        plugin_manager_.UninstallAll(*this);

        {
            std::lock_guard<std::mutex> lock(bridge_mutex_);
            bridge_map_.clear();
        }

        CHWELL_LOG_INFO("Service stopped (graceful shutdown complete)");
    }

    // 🆕 设置 graceful shutdown 时排空 LogicThread 队列的超时（毫秒）
    void set_shutdown_drain_timeout_ms(int ms) { shutdown_drain_timeout_ms_ = ms; }

    void PreShut() {
        CHWELL_LOG_INFO("Service PreShut: notifying components...");
        for (auto it = initialized_components_.rbegin(); it != initialized_components_.rend(); ++it) {
            cleanup_call(*it, true);
        }
    }

    void Shut() {
        CHWELL_LOG_INFO("Service Shut: releasing resources...");
        for (auto it = initialized_components_.rbegin(); it != initialized_components_.rend(); ++it) {
            cleanup_call(*it, false);
            component_records_.at(*it).needs_shutdown = false;
        }
        initialized_components_.clear();
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
    // 兼容旧接口：返回分片 0（DB/RPC 回调建议用 logic_pool().post_task_for）
    net::LogicThread* logic_thread() { return logic_pool_ ? logic_pool_->worker(0) : nullptr; }
    net::LogicThreadPool* logic_pool() { return logic_pool_.get(); }
    bool is_running() const { return running_; }
    bool use_epoll() const { return use_epoll_; }
    PluginManager& plugin_manager() { return plugin_manager_; }

private:
    friend class PluginManager;
    struct ComponentRecord {
        const IPlugin* owner;
        std::optional<int> priority;
        bool needs_shutdown;
    };

    Component* registration_failed(const std::string& message) {
        registration_error_ = true;
        CHWELL_LOG_ERROR(message);
        return nullptr;
    }

    void cleanup_call(Component* component, bool pre_shut) noexcept {
        try {
            bool ok = pre_shut ? component->PreShut() : component->Shut();
            if (!ok) CHWELL_LOG_ERROR("Component cleanup failed: " << component->name());
        } catch (...) {
            CHWELL_LOG_ERROR("Component cleanup threw");
        }
    }

    void remove_component(Component* component) {
        auto record = component_records_.find(component);
        if (record != component_records_.end() && record->second.needs_shutdown) {
            cleanup_call(component, true);
            cleanup_call(component, false);
        }
        component_records_.erase(component);
        components_.erase(std::remove_if(components_.begin(), components_.end(),
            [component](const std::unique_ptr<Component>& comp) { return comp.get() == component; }),
            components_.end());
    }

    void remove_owned_components(const IPlugin* owner) {
        std::vector<Component*> owned;
        for (auto it = components_.rbegin(); it != components_.rend(); ++it) {
            if (component_records_.at(it->get()).owner == owner) owned.push_back(it->get());
        }
        for (auto* component : owned) remove_component(component);
    }

    void shutdown_owned_components(const IPlugin* owner) {
        for (auto it = components_.rbegin(); it != components_.rend(); ++it) {
            auto& record = component_records_.at(it->get());
            if (record.owner == owner && record.needs_shutdown) {
                cleanup_call(it->get(), true);
                cleanup_call(it->get(), false);
                record.needs_shutdown = false;
            }
        }
    }

    void cleanup_start_failure() {
        if (network_attempted_) {
            if (use_epoll_) epoll_server_->stop();
            else {
                legacy_server_->stop();
                io_service_.stop();
                thread_pool_.stop();
            }
            stopped_after_run_ = true;
        }
        if (logic_pool_) logic_pool_->stop();
        running_ = false;
        PreShut();
        Shut();
        plugin_manager_.UninstallAll(*this);
        starting_ = false;
    }

    /**
     * @brief 分发消息到 Component
     *
     * epoll 模式：投递到 LogicThread，保证单线程消费
     * legacy 模式：直接调用（legacy 本身是单线程）
     */
    void dispatch_message(const net::TcpConnectionPtr& conn,
                          std::string_view data) {
        if (logic_pool_) {
            // epoll 模式：按 conn_id 分片投递，同连接保序
            logic_pool_->post(conn, data);
        } else {
            // legacy 模式：直接调用
            for (auto& comp : components_) comp->on_message(conn, data);
        }
    }

    /**
     * @brief 分发断连事件到 Component
     */
    void dispatch_disconnect(const net::TcpConnectionPtr& conn) {
        if (logic_pool_) {
            logic_pool_->post_disconnect(conn);
        } else {
            for (auto& comp : components_) comp->on_disconnect(conn);
        }
    }

    bool use_epoll_;
    // 必须先声明 unique_ptr 再绑定引用（C++ 按声明顺序初始化）
    std::unique_ptr<net::IoService> io_service_ptr_;
    net::IoService& io_service_;
    std::unique_ptr<net::TcpServer> legacy_server_;
    std::unique_ptr<net::EpollTcpServer> epoll_server_;

    // LogicThreadPool：epoll 模式下按 conn_id 分片的多 worker 逻辑消费
    std::unique_ptr<net::LogicThreadPool> logic_pool_;

    // epoll 模式下：fd → bridge 的映射
    std::mutex bridge_mutex_;
    std::unordered_map<std::uint64_t, net::TcpConnectionPtr> bridge_map_;

    core::ThreadPool thread_pool_;
    std::size_t worker_threads_;
    std::vector<std::unique_ptr<Component>> components_;
    std::unordered_map<const Component*, ComponentRecord> component_records_;
    std::vector<Component*> initialized_components_;
    const IPlugin* registration_owner_ = nullptr;
    bool registration_error_ = false;
    bool registration_blocked_ = false;
    bool starting_ = false;
    bool network_attempted_ = false;
    bool stopped_after_run_ = false;

    PluginManager plugin_manager_;
    std::atomic<bool> running_;
    std::chrono::steady_clock::time_point last_update_time_;
    int shutdown_drain_timeout_ms_ = 5000;  // 🆕 默认 5 秒排空超时
};

} // namespace service
} // namespace chwell
