#include "chwell/rpc/rpc_server.h"
#include "chwell/protocol/parser.h"
#include "chwell/core/logger.h"
#include "chwell/service/service.h"

namespace chwell {
namespace rpc {

RpcServer::RpcServer(net::IoService& io_service, unsigned short port)
    : io_service_(io_service)
    , port_(port) {
    server_ = std::make_unique<net::TcpServer>(io_service_, port_);
}

RpcServer::~RpcServer() {
    stop();
}

void RpcServer::register_method(std::uint16_t method_id, RpcHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    methods_[method_id] = std::move(handler);
    CHWELL_LOG_INFO("RPC method registered: " << method_id);
}

void RpcServer::unregister_method(std::uint16_t method_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    methods_.erase(method_id);
}

void RpcServer::start() {
    server_->set_connection_callback([this](const net::TcpConnectionPtr& conn) {
        handle_connect(conn);
    });
    
    server_->set_disconnect_callback([this](const net::TcpConnectionPtr& conn) {
        handle_disconnect(conn);
    });
    
    server_->set_message_callback([this](const net::TcpConnectionPtr& conn,
                                          std::string_view data) {
        handle_message(conn, data);
    });
    
    server_->start_accept();
    CHWELL_LOG_INFO("RpcServer started on port " << port_);
}

void RpcServer::stop() {
    server_->stop();
    CHWELL_LOG_INFO("RpcServer stopped");
}

void RpcServer::handle_connect(const net::TcpConnectionPtr& conn) {
    active_connections_.fetch_add(1);
    CHWELL_LOG_DEBUG("RPC client connected, active=" << active_connections_.load());
}

void RpcServer::handle_disconnect(const net::TcpConnectionPtr& conn) {
    active_connections_.fetch_sub(1);
    
    std::lock_guard<std::mutex> lock(mutex_);
    buffers_.erase(conn.get());
    
    CHWELL_LOG_DEBUG("RPC client disconnected, active=" << active_connections_.load());
}

void RpcServer::handle_message(const net::TcpConnectionPtr& conn, std::string_view data) {
    // 累积数据到缓冲区
    std::vector<char> buffer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& buf = buffers_[conn.get()];
        buf.insert(buf.end(), data.begin(), data.end());
        buffer = buf;  // 复制一份用于解析
    }

    // 解析消息
    protocol::Parser parser;
    auto messages = parser.feed(buffer);
    
    for (const auto& msg : messages) {
        total_requests_.fetch_add(1);
        
        // 查找处理器
        RpcHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = methods_.find(msg.cmd);
            if (it != methods_.end()) {
                handler = it->second;
            }
        }
        
        // Extract request_id prefix (first 4 bytes of body, big-endian)
        // and strip it before passing to handler; echo it back in response.
        std::vector<char> handler_body;
        std::vector<char> request_id_prefix;
        if (msg.body.size() >= 4) {
            request_id_prefix.assign(msg.body.begin(), msg.body.begin() + 4);
            handler_body.assign(msg.body.begin() + 4, msg.body.end());
        } else {
            handler_body = msg.body;
        }

        if (handler) {
            // 处理请求
            std::vector<char> handler_response;
            try {
                handler(handler_body, handler_response);
            } catch (const std::exception& e) {
                CHWELL_LOG_ERROR("RPC handler exception: " << e.what());
                handler_response = std::vector<char>(e.what(), e.what() + strlen(e.what()));
            }

            // 在响应 body 前缀中回传 request_id
            std::vector<char> response;
            response.insert(response.end(), request_id_prefix.begin(), request_id_prefix.end());
            response.insert(response.end(), handler_response.begin(), handler_response.end());

            protocol::Message resp_msg(msg.cmd, response);
            auto serialized = protocol::serialize(resp_msg);
            conn->send(serialized);
        } else {
            // 方法未找到，仍需回传 request_id 使客户端能匹配
            CHWELL_LOG_WARN("RPC method not found: " << msg.cmd);
            protocol::Message resp_msg(msg.cmd, request_id_prefix);
            auto serialized = protocol::serialize(resp_msg);
            conn->send(serialized);
        }
    }
    
    // 更新缓冲区（移除已处理的数据）
    if (!messages.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& buf = buffers_[conn.get()];
        
        // 计算已消费的字节数
        size_t consumed = 0;
        for (const auto& msg : messages) {
            consumed += 4 + msg.body.size();  // cmd(2) + len(2) + body
        }
        
        if (consumed <= buf.size()) {
            buf.erase(buf.begin(), buf.begin() + consumed);
        }
    }
}

//=============================================================================
// RpcServerComponent 实现
//=============================================================================

RpcServerComponent::RpcServerComponent(unsigned short port)
    : port_(port) {
}

void RpcServerComponent::on_register(service::Service& svc) {
    server_ = std::make_unique<RpcServer>(svc.io_service(), port_);
    server_->start();
}

} // namespace rpc
} // namespace chwell