#pragma once

#include <string_view>
#include <vector>
#include <memory>
#include <type_traits>
#include <algorithm>
#include <chrono>

#include "chwell/core/thread_pool.h"
#include "chwell/core/logger.h"
#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_server.h"
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
 * 
 * 生命周期：
 * Init → PostInit → CheckConfig → PreUpdate → Update(循环) → PreShut → Shut
 */
class Service {
public:
    Service(unsigned short listen_port, std::size_t worker_threads)
        : server_(io_service_, listen_port),
          thread_pool_(worker_threads),
          worker_threads_(worker_threads),
          running_(false) {
        server_.set_connection_callback([](const net::TcpConnectionPtr& conn) {
            (void)conn;
            CHWELL_LOG_INFO("New connection");
        });

        server_.set_disconnect_callback([this](const net::TcpConnectionPtr& conn) {
            CHWELL_LOG_INFO("Connection closed");
            dispatch_disconnect(conn);
        });

        server_.set_message_callback([this](const net::TcpConnectionPtr& conn,
                                            std::string_view data) {
            dispatch_message(conn, data);
        });
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
    
    // 阶段1: 初始化所有组件
    bool Init() {
        CHWELL_LOG_INFO("Service Init: initializing components...");
        
        // 按优先级排序
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
    
    // 阶段2: 后初始化（建立依赖关系）
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
    
    // 阶段3: 检查配置
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
    
    // 阶段4: 更新前准备
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
        // 完整的生命周期流程
        if (!Init()) {
            CHWELL_LOG_ERROR("Service Init failed, aborting...");
            return;
        }
        
        if (!PostInit()) {
            CHWELL_LOG_ERROR("Service PostInit failed, aborting...");
            return;
        }
        
        if (!CheckConfig()) {
            CHWELL_LOG_ERROR("Service CheckConfig failed, aborting...");
            return;
        }
        
        if (!PreUpdate()) {
            CHWELL_LOG_ERROR("Service PreUpdate failed, aborting...");
            return;
        }
        
        // 启动网络
        server_.start_accept();

        for (std::size_t i = 0; i < worker_threads_; ++i) {
            thread_pool_.post([this]() {
                io_service_.run();
            });
        }

        running_ = true;
        last_update_time_ = std::chrono::steady_clock::now();
        
        CHWELL_LOG_INFO("Service started successfully");
    }

    void stop() {
        if (!running_) return;
        
        CHWELL_LOG_INFO("Service stopping...");
        
        // 阶段6: 关闭前清理
        PreShut();
        
        // 停止网络
        server_.stop();
        io_service_.stop();
        
        // 阶段7: 关闭
        Shut();
        
        // 卸载插件
        plugin_manager_.UninstallAll(*this);
        
        running_ = false;
        CHWELL_LOG_INFO("Service stopped");
    }
    
    // 阶段6: 关闭前清理
    void PreShut() {
        CHWELL_LOG_INFO("Service PreShut: notifying components...");
        
        for (auto& comp : components_) {
            comp->PreShut();
        }
    }
    
    // 阶段7: 关闭
    void Shut() {
        CHWELL_LOG_INFO("Service Shut: releasing resources...");
        
        for (auto& comp : components_) {
            comp->Shut();
        }
    }
    
    // ========== 主循环更新 ==========
    
    void Update() {
        if (!running_) return;
        
        auto now = std::chrono::steady_clock::now();
        auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_update_time_).count();
        last_update_time_ = now;
        
        // 调用所有组件的 Update
        for (auto& comp : components_) {
            comp->Update(delta_ms);
        }
    }

    // ========== 访问器 ==========
    
    net::IoService& io_service() { return io_service_; }
    net::TcpServer& tcp_server() { return server_; }
    bool is_running() const { return running_; }
    PluginManager& plugin_manager() { return plugin_manager_; }

private:
    void dispatch_message(const net::TcpConnectionPtr& conn,
                          std::string_view data) {
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
    
    PluginManager plugin_manager_;
    bool running_;
    std::chrono::steady_clock::time_point last_update_time_;
};

} // namespace service
} // namespace chwell
