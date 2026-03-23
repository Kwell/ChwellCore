#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <unordered_map>
#include <memory>
#include <mutex>

#include "chwell/net/posix_io.h"
#include "chwell/net/tcp_server.h"
#include "chwell/service/component.h"
#include "chwell/protocol/message.h"

namespace chwell {
namespace rpc {

// RPC 处理器类型
typedef std::function<void(const std::vector<char>& request, std::vector<char>& response)> RpcHandler;

// RPC 服务端
class RpcServer {
public:
    RpcServer(net::IoService& io_service, unsigned short port);
    ~RpcServer();
    
    // 注册 RPC 方法
    void register_method(std::uint16_t method_id, RpcHandler handler);
    
    // 取消注册
    void unregister_method(std::uint16_t method_id);
    
    // 启动服务
    void start();
    
    // 停止服务
    void stop();
    
    // 获取统计
    int total_requests() const { return total_requests_.load(); }
    int active_connections() const { return active_connections_.load(); }
    
private:
    void handle_message(const net::TcpConnectionPtr& conn, std::string_view data);
    void handle_connect(const net::TcpConnectionPtr& conn);
    void handle_disconnect(const net::TcpConnectionPtr& conn);
    
    net::IoService& io_service_;
    std::unique_ptr<net::TcpServer> server_;
    unsigned short port_;
    
    std::mutex mutex_;
    std::unordered_map<std::uint16_t, RpcHandler> methods_;
    std::unordered_map<const net::TcpConnection*, std::vector<char>> buffers_;
    
    std::atomic<int> total_requests_{0};
    std::atomic<int> active_connections_{0};
};

// RPC 服务组件（可挂载到 Service）
class RpcServerComponent : public service::Component {
public:
    explicit RpcServerComponent(unsigned short port);
    virtual ~RpcServerComponent() = default;
    
    virtual std::string name() const override {
        return "RpcServerComponent";
    }
    
    virtual void on_register(service::Service& svc) override;
    
    void register_method(std::uint16_t method_id, RpcHandler handler) {
        if (server_) {
            server_->register_method(method_id, std::move(handler));
        }
    }
    
    void unregister_method(std::uint16_t method_id) {
        if (server_) {
            server_->unregister_method(method_id);
        }
    }
    
private:
    unsigned short port_;
    std::unique_ptr<RpcServer> server_;
};

// RPC 方法注册辅助宏
#define RPC_REGISTER_METHOD(server, method_id, handler) \
    (server).register_method(method_id, handler)

// 简化的 RPC 方法定义
// 用法：RPC_METHOD(handle_login, request, response) { ... }
#define RPC_METHOD(name, request, response) \
    void name(const std::vector<char>& request, std::vector<char>& response)

} // namespace rpc
} // namespace chwell