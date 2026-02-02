## ChwellFrameCore - 分布式游戏后端基础框架（C++11）

### 特性概览
- 核心目标：为分布式游戏服务器提供一个**可扩展、可定制的基础骨架**，专注连接管理、线程模型和节点抽象。
- 基于 **C++11** 与 **Asio**（使用 header-only 形式，兼容 Windows）。
- 提供：
  - **日志模块**：简单线程安全控制台日志。
  - **配置模块**：当前为内建默认值（可扩展为 JSON/INI）。
  - **线程池模块**：用于驱动 `io_service` 或业务任务。
  - **网络层**：异步 TCP 服务器、会话封装、消息回调。
  - **集群层骨架**：节点抽象，便于后续扩展心跳/注册/RPC。
  - **示例 Echo 服务器**：最小可跑示例，方便你继续迭代。

### 目录结构
- `include/chwell/core`：核心模块（`logger`, `config`, `thread_pool`）
- `include/chwell/net`：网络层（`tcp_server`, `tcp_connection`）
- `include/chwell/cluster`：集群节点抽象（`node`）
- `src/core`：核心模块实现
- `src/net`：网络层实现
- `src/cluster`：集群层实现
- `examples`：示例服务（`echo_server`）

### 构建步骤（使用 CMake）

#### 1. 准备 Asio 头文件（header-only）
- 从 Asio 官方仓库或发行包中获取 Asio 源码（只需头文件部分）。
- 将 Asio 的 `asio` 目录（或其上层 `include` 目录）放到本工程的 `3rdparty/asio/include` 下：
  - 也就是说，最终应当能通过 `3rdparty/asio/include/asio.hpp` 被找到。

#### 2. 配置与编译
- `cd build`
- `cmake ..`
- `cmake --build . --config Release`

生成的示例可执行文件为：
- `example_echo_server`（基础组件示例）
- `example_protocol_server`（组件 + 协议路由示例）
- `example_http_server`（简单 HTTP Server 示例）

### 运行示例服务器

在 `build` 目录（或相应输出目录）执行：

- `./example_echo_server` （Windows 下为 `example_echo_server.exe`）  
  - 纯粹的 Echo 示例，展示最基础的组件化 Service 用法。
- `./example_protocol_server` （Windows 下为 `example_protocol_server.exe`）  
  - 使用 **协议路由组件 + 多业务组件**，展示真实项目更接近的写法。

默认监听端口：`9000`，工作线程数：`4`。  
你可以使用任意 TCP 客户端连接该端口：
- 在 `example_echo_server` 下，发什么就回什么。
- 在 `example_protocol_server` 下，按照简单协议 `cmd + len + body` 与服务交互。
 - 在 `example_http_server` 下，通过浏览器或 curl 访问 `http://127.0.0.1:8080/`、`/health`。

### 组件与协议路由使用说明

- **组件基类 `Component`**
  - 必须实现：`std::string name() const`
  - 可选实现：`on_register(Service&)`、`on_message(conn, data)`、`on_disconnect(conn)`
  - 下游只需要继承 `Component` 并实现这些接口，即可作为“功能插件”挂在某个 `Service` 上。

- **服务类 `Service`**
  - 构造：`Service(unsigned short listen_port, std::size_t worker_threads)`
  - 注册组件：`add_component<T>(args...)`，返回组件指针。
  - 获取组件：`get_component<T>()`，按类型查找已注册的组件。
  - 生命周期：`start()` / `stop()`

- **协议层**
  - 消息格式：`| cmd(uint16) | len(uint16) | body(len bytes) |`（均为网络字节序）
  - `protocol::Message`： `{ cmd, body }`
  - `protocol::serialize / deserialize`：负责编解码。
  - `protocol::Parser`：负责粘包/拆包。

- **协议路由组件 `ProtocolRouterComponent`**
  - 注册路由：`register_handler(cmd, handler)`，其中 `handler(conn, Message)`。
  - 发送消息：`ProtocolRouterComponent::send_message(conn, Message)`。
  - 内部自动为每个连接维护 `Parser`，在 `on_message` 中解析并根据 `cmd` 分发。

### 已实现的高级功能

#### 1. 协议编解码层 (`chwell/codec`)
- **`Codec` 接口**：统一的编解码抽象
- **`LengthHeaderCodec`**：长度头编解码器（4字节长度 + body）
- **`JsonCodec` / `ProtobufCodec`**：占位实现，可扩展为真实JSON/Protobuf编解码

#### 2. 增强会话管理 (`chwell/service/session_manager.h`)
- **`SessionManager`**：支持玩家ID、房间ID、网关ID绑定
- 提供 `login()`, `logout()`, `join_room()`, `leave_room()` 等接口
- 支持按房间查询玩家列表、更新活跃时间等

#### 3. 集群与RPC (`chwell/cluster`, `chwell/rpc`)
- **`NodeRegistry`**：节点注册表，支持节点注册、注销、按类型查找
- **`RpcClient`**：基于 `TcpConnection` 的RPC客户端封装，支持异步/同步调用

#### 4. 可靠性保障 (`chwell/reliability`)
- **`HeartbeatManager`**：心跳管理器，定期检测超时连接
- **`RateLimiter`**：令牌桶限流器，支持全局和按连接限流
- **`Metrics`**：监控指标收集器（QPS、在线数、延迟统计）

### 使用示例

#### 使用编解码器
```cpp
codec::LengthHeaderCodec codec;
std::vector<char> encoded = codec.encode("Hello World");
std::vector<std::string> decoded = codec.decode(encoded);
```

#### 使用SessionManager
```cpp
auto* session_mgr = svc.add_component<service::SessionManager>();
session_mgr->login(conn, "player123");
session_mgr->join_room(conn, "room456");
std::vector<std::string> players = session_mgr->get_players_in_room("room456");
```

#### 使用RPC客户端
```cpp
rpc::RpcClient rpc_client(io_service);
rpc_client.connect("127.0.0.1", 9001);
rpc_client.call(100, request_data, [](const protocol::Message& resp) {
    // 处理响应
});
```

#### 使用监控指标
```cpp
reliability::Metrics::instance().increment_qps();
reliability::Metrics::instance().increment_online();
reliability::Metrics::instance().record_latency("rpc_call", 50);
```

你可以把这里作为一个**功能完整的基础框架**，按你游戏的实际需求逐步补强业务逻辑、网关服、房间服等模块。

