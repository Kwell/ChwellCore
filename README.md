# ChwellCore — 游戏后端核心框架

模块化、高性能的 C++17 游戏服务器框架，专为 SLG / MMO 等中大型在线游戏设计。

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.11+-brightgreen.svg)](https://cmake.org/)

---

## 目录

- [特性概览](#特性概览)
- [快速开始](#快速开始)
- [架构设计](#架构设计)
- [核心模块](#核心模块)
- [代码示例](#代码示例)
- [游戏协议](#游戏协议)
- [构建选项](#构建选项)
- [测试](#测试)
- [配置文件](#配置文件)
- [目录结构](#目录结构)
- [路线图](#路线图)
- [许可证](#许可证)

---

## 特性概览

| 类别 | 功能 |
|------|------|
| **网络** | POSIX 非阻塞 I/O（`poll`），TCP / UDP / WebSocket / HTTP，TLS（OpenSSL 可选），连接池 |
| **协议** | 自定义二进制帧 `[cmd:2B][len:2B][body]`，Protobuf 帧，JSON 帧，流式粘包解析器 |
| **服务层** | 组件化 `Service` 容器，按命令字路由，`SessionManager` 多维会话映射 |
| **同步** | `FrameSyncRoom`（帧同步 + 快照），`StateSyncRoom`（K/V 状态 + 增量差异 + 订阅） |
| **游戏组件** | 登录、聊天、房间、心跳、玩家移动（均可直接插拔到 `Service`）|
| **基础设施** | 时间轮定时器（O(1)），线程池，任务队列（延时/重复/取消），对象池，类型安全事件总线 |
| **空间** | 格子 AOI、十字链表 AOI，SLG 地图与战斗系统 |
| **存储** | 统一 KV 接口（内存 / MySQL / MongoDB），模板化 ORM `Repository<T>`，YAML 配置驱动 |
| **集群** | 节点注册表（YAML + 一致性哈希），轻量 RPC，网关转发 |
| **可靠性** | 熔断器（计数 / 失败率 / 混合策略），令牌桶 / 漏桶 / 固定窗口限流，Prometheus 指标 |
| **Redis** | 同步 Redis 客户端，分布式锁（SETNX + 自动续租 + RAII 守卫）|
| **Benchmark** | 内置 `BenchmarkSuite`，支持预热 + 多次采样 + 统计导出（CSV / JSON）|

---

## 快速开始

### 安装系统依赖（Ubuntu/Debian）

```bash
sudo apt install build-essential cmake libyaml-cpp-dev libssl-dev
```

### 克隆并编译

```bash
git clone <repo_url> ChwellCore
cd ChwellCore
cmake -B build -DCHWELL_BUILD_TESTS=ON -DCHWELL_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
```

### 运行示例

```bash
cd build

./example_echo_server              # Echo 服务器（端口 9000）
./example_protocol_server          # 协议路由服务器
./example_http_server              # HTTP 服务器
./example_sync_demo                # 帧同步 + 状态同步演示
./example_game_components_demo     # 游戏组件（TCP）
./example_game_ws_components_demo  # 游戏组件（WebSocket）
./example_storage && ./example_orm # 存储 + ORM
```

### H5 对战小游戏 Demo

完整的浏览器端对战 Demo（登录 → 大厅 → 房间 → 回合制对战），参见 [examples/h5_game/README.md](examples/h5_game/README.md)。

```bash
# 终端 1
./example_game_server

# 终端 2
./example_game_gateway_server

# 终端 3（需 OpenSSL）
./ws_tcp_bridge

# 浏览器
cd ../examples/h5_game && python3 -m http.server 8080
# 访问 http://localhost:8080
```

---

## 架构设计

```
+---------------------------------------------------------------+
|                     应用层 (Application)                      |
|  Game Components  ·  FrameSync / StateSync  ·  Gateway / RPC  |
+---------------------------------------------------------------+
                               │
+---------------------------------------------------------------+
|                      服务层 (Service)                         |
|       Service 容器  ·  Component 系统  ·  SessionManager      |
|       ProtocolRouterComponent  ·  EventBus                    |
+---------------------------------------------------------------+
                               │
+---------------------------------------------------------------+
|                      协议层 (Protocol)                        |
|    Protocol Parser（粘包）  ·  Message（cmd+body）            |
|    Codec: LengthHeader / Protobuf / JSON                      |
+---------------------------------------------------------------+
                               │
+---------------------------------------------------------------+
|                      网络层 (Network)                         |
|    posix_io (poll / 非阻塞)                                   |
|    TcpServer / TcpConnection / UdpSocket / WsServer / Http    |
|    ConnectionPool  ·  TLS（可选）                             |
+---------------------------------------------------------------+
                               │
+---------------------------------------------------------------+
|                     基础设施 (Infrastructure)                 |
|  ThreadPool · TimerWheel · TaskQueue · ObjectPool             |
|  Storage (Memory/MySQL/MongoDB) · Redis · DistributedLock     |
|  ServiceDiscovery · LoadBalancer · CircuitBreaker             |
|  Metrics · RateLimit · AOI · SlgMap · ConsistentHash          |
+---------------------------------------------------------------+
```

### 组件化设计

`Service` 容器持有一组 `Component`，消息、连接、断线事件自动广播至每个组件：

```cpp
service::Service svc(9000, /*worker_threads=*/4);

svc.add_component<service::SessionManager>();
svc.add_component<service::ProtocolRouterComponent>();
svc.add_component<game::LoginComponent>();
svc.add_component<game::ChatComponent>();
svc.add_component<game::RoomComponent>();
svc.add_component<game::HeartbeatComponent>();

svc.start();
std::cin.get();
svc.stop();
```

自定义组件只需继承 `Component` 并覆写所需回调：

```cpp
class MyComponent : public chwell::service::Component {
public:
    std::string name() const override { return "MyComponent"; }

    void on_message(const chwell::net::TcpConnectionPtr& conn,
                    const std::vector<char>& data) override {
        // 处理消息
    }

    void on_disconnect(const chwell::net::TcpConnectionPtr& conn) override {
        // 连接断开时清理
    }
};
```

---

## 核心模块

### 网络层 (`chwell/net`)

| 类 | 说明 |
|----|------|
| `TcpServer` | 监听端口，accept 新连接，管理连接生命周期 |
| `TcpConnection` | TCP 非阻塞读写缓冲与回调 |
| `UdpSocket` | UDP 非阻塞收发；`bind_udp_port` 绑定本地端口 |
| `UdpServer` | UDP 服务端封装 |
| `WsServer` / `WsConnection` | WebSocket 握手（SHA-1）与文本 / 二进制帧收发 |
| `HttpServer` | 简易 HTTP 路由服务 |
| `ConnectionPool` | TCP 连接池（借出/归还） |
| `TlsContext` / `TlsConnection` | OpenSSL TLS 包装（`CHWELL_USE_OPENSSL=ON`）|
| `IConnection` | TCP/WebSocket 统一接口（`connection_adapter.h`）|

> **连接接口层次**：`connection_adapter.h` 中的 `IConnection` 为主用接口，提供 `send / send_text / close / type`；`connection.h` 提供仅含 `native_handle + description` 的轻量基类 `IBaseConnection`；`net_interface.h` 提供面向服务端的 `INetConnection / IServer` 抽象。

### 服务层 (`chwell/service`)

| 类 | 说明 |
|----|------|
| `Service` | 组件容器，持有 `TcpServer` 和 `ThreadPool` |
| `Component` | 组件基类，`on_register / on_message / on_disconnect` |
| `ProtocolRouterComponent` | 按 cmd 查表并调用 `MessageHandler` |
| `SessionManager` | 连接 → 玩家 ID / 房间 ID / 网关 ID 多维映射 |

### 游戏组件 (`chwell/game`)

| 组件 | 说明 |
|------|------|
| `LoginComponent` | 登录/登出，依赖 `SessionManager` |
| `ChatComponent` | 消息广播到同房间所有玩家 |
| `RoomComponent` | 房间创建/加入/离开 |
| `HeartbeatComponent` | 心跳保活 |
| `PlayerMoveComponent` | 位置同步，依赖 AOI 广播 |

### 同步系统 (`chwell/sync`)

**帧同步**（`FrameSyncRoom`）：管理每帧的玩家输入队列（`submit_input / get_all_inputs`）、帧快照（`create_snapshot / get_snapshot`）、`all_inputs_ready` 检测，`FrameSyncComponent` 与 `Service` 一体化集成。

**状态同步**（`StateSyncRoom`）：支持 int32 / int64 / float / double / string / binary 六种值类型，提供 `update_state / query_state / create_snapshot / subscribe` 接口，增量差异（`StateDiff`）+ 全量快照（`StateSnapshot`）推送。

### 编解码 (`chwell/codec`)

| 编解码器 | 帧格式 | 适用场景 |
|----------|--------|----------|
| 内置协议（`protocol/`）| `[cmd:2B BE][len:2B BE][body]` | 游戏服默认协议 |
| `ProtobufCodec` | `[varint32 len][pb payload]` | 纯 Protobuf 消息 |
| `JsonCodec` | `[len:4B BE][json bytes]` | 调试 / HTTP 风格接口 |

### 基础设施

| 模块 | 关键类 | 说明 |
|------|--------|------|
| `chwell/core` | `TimerWheel` | 分层时间轮，O(1) 添加/取消定时器 |
| `chwell/core` | `ThreadPool` | 固定大小线程池，`post()` 提交任务 |
| `chwell/task` | `DelayedTaskQueue` | 支持延时 / 重复 / 取消的任务队列 |
| `chwell/pool` | `ObjectPool<T>` | 模板对象池；`GlobalBufferPool` 全局缓冲区 |
| `chwell/event` | `EventBus` | 类型安全发布/订阅，线程安全，支持优先级 |

### 存储层 (`chwell/storage`)

**同步接口：**

```cpp
// 从 YAML 配置创建存储实例（type: memory | mysql | mongodb）
auto store = storage::StorageFactory::create_from_yaml("config/storage.yaml");
store->put("player:123", data);
auto r = store->get("player:123");  // r.ok, r.value

// ORM 仓储
storage::orm::Repository<Player> repo(store.get(), "players");
repo.save(player);
auto p = repo.find("player123");    // std::unique_ptr<Player>
auto all = repo.find_all();         // std::vector<std::unique_ptr<Player>>
```

**异步接口（`chwell/storage/async_storage_adapter.h`）：**

`AsyncStorageAdapter` 包装任意 `StorageInterface`，内置线程池，提供 Future 和 Callback 两种风格：

```cpp
#include "chwell/storage/async_storage_adapter.h"

storage::MemoryStorage mem;
storage::AsyncStorageAdapter async_store(&mem, /*num_threads=*/4);

// Future 风格
auto f = async_store.async_put("key", "value");
f.get();  // 等待完成

auto f_get = async_store.async_get("key");
auto result = f_get.get();   // result.ok, result.value

// 批量操作
auto f_mget = async_store.async_mget({"k1", "k2", "k3"});
auto results = f_mget.get(); // std::vector<StorageResult>

// Callback 风格
async_store.async_get("key", [](storage::StorageResult r) {
    if (r.ok) { /* 使用 r.value */ }
});

async_store.async_exists("key", [](bool exists) {
    // 处理结果
});
```

### 集群与 RPC (`chwell/cluster`, `chwell/rpc`)

```cpp
// RPC 服务端
rpc::RpcServer server(9090);
server.register_handler("echo", [](const std::string& req, std::string& resp) {
    resp = req;
    return true;
});
server.start();

// RPC 客户端（含超时与熔断器集成）
rpc::RpcClient client("127.0.0.1", 9090);
std::string resp;
bool ok = client.call_sync("echo", "hello", resp, /*timeout_ms=*/1000);
```

### 可靠性 (`chwell/circuitbreaker`, `chwell/ratelimit`, `chwell/metrics`)

```cpp
// 熔断器（失败次数策略）
circuitbreaker::CircuitBreakerConfig cfg;
cfg.trip_strategy = circuitbreaker::TripStrategy::FAILURE_COUNT;
cfg.failure_threshold = 5;
circuitbreaker::DefaultCircuitBreaker cb("svc", cfg);
auto result = cb.execute([&]() { remote_call(); });

// 熔断器（失败率策略）：failure_threshold 为最小样本数，达到后才按失败率判断
circuitbreaker::CircuitBreakerConfig rate_cfg;
rate_cfg.trip_strategy       = circuitbreaker::TripStrategy::FAILURE_RATE;
rate_cfg.failure_threshold   = 10;   // 至少 10 次调用后才计算失败率
rate_cfg.failure_rate_threshold = 0.5;

// 限流器（令牌桶）
ratelimit::TokenBucketRateLimiter limiter(1000, 100); // 1000 QPS，突发 100
if (!limiter.try_acquire()) { /* 限流 */ }

// Prometheus 指标
auto& counter = metrics::Counter::get_or_create("requests_total");
counter.inc();
```

### Redis 与分布式锁 (`chwell/redis`)

```cpp
redis::RedisClient redis("127.0.0.1", 6379);
redis.set("key", "value");
auto val = redis.get("key");

// RAII 分布式锁（内部自动续租，离开作用域自动解锁）
redis::DistributedLock lock(redis, "resource:123", /*ttl_ms=*/5000);
if (lock.try_lock()) {
    // 临界区
}
```

### Benchmark 框架 (`chwell/benchmark`)

```cpp
chwell::benchmark::BenchmarkSuite suite("MyBench");

suite.add_benchmark("vector_push", "push_back 10000 int", []() {
    std::vector<int> v;
    for (int i = 0; i < 10000; ++i) v.push_back(i);
});

chwell::benchmark::BenchmarkConfig cfg;
cfg.warmup_iterations       = 100;
cfg.measurement_iterations  = 1000;

auto results = suite.run(cfg);
suite.print_results();
std::string csv = suite.export_csv();  // 或 export_json()
```

---

## 代码示例

### Echo 服务器

```cpp
#include "chwell/service/service.h"
#include "chwell/service/component.h"

class EchoComponent : public chwell::service::Component {
public:
    std::string name() const override { return "EchoComponent"; }

    void on_message(const chwell::net::TcpConnectionPtr& conn,
                    const std::vector<char>& data) override {
        conn->send(data);
    }
};

int main() {
    chwell::service::Service svc(9000, 2);
    svc.add_component<EchoComponent>();
    svc.start();
    std::cin.get();
    return 0;
}
```

### 协议路由服务器

```cpp
#include "chwell/service/service.h"
#include "chwell/service/session_manager.h"
#include "chwell/service/protocol_router.h"

int main() {
    chwell::service::Service svc(9000, 4);

    auto* router = svc.add_component<chwell::service::ProtocolRouterComponent>();
    svc.add_component<chwell::service::SessionManager>();

    router->register_handler(0x0001,
        [](const chwell::net::TcpConnectionPtr& conn,
           const chwell::protocol::Message& msg) {
            chwell::protocol::Message resp(0x0002, "login ok");
            chwell::service::ProtocolRouterComponent::send_message(conn, resp);
        });

    svc.start();
    std::cin.get();
    return 0;
}
```

### 时间轮定时器

```cpp
#include "chwell/core/timer_wheel.h"

chwell::core::TimerWheel wheel(/*tick_ms=*/100, /*max_slots=*/60, /*layers=*/4);
wheel.start();

// 一次性定时器（500ms 后触发）
auto h = wheel.add_timer(500, []() { /* 回调 */ });

// 重复定时器（每 1000ms 触发一次）
auto r = wheel.add_repeat_timer(1000, []() { /* 周期任务 */ });

wheel.cancel_timer(r);  // 取消
wheel.stop();
```

### 对象池

```cpp
#include "chwell/pool/object_pool.h"

chwell::pool::ObjectPool<Bullet> pool;

{
    auto bullet = pool.acquire();  // 借出
    bullet->x = 100;
    // 离开作用域自动归还
}
```

### AOI

```cpp
#include "chwell/aoi/aoi.h"

chwell::aoi::CrossListAoi aoi;
aoi.add_entity(entity_id, x, y, view_range);
auto visible = aoi.get_entities_in_view(entity_id);
```

---

## 游戏协议

### 帧格式

```
+----------+----------+---------------------------+
|  cmd     |  len     |  body                     |
|  2 字节  |  2 字节  |  len 字节                 |
|  大端序  |  大端序  |  业务数据                 |
+----------+----------+---------------------------+
```

### 内置命令字

**游戏组件（`game_components.h`）**

| 命令字 | 名称 | 方向 |
|--------|------|------|
| 0x0001 | C2S_LOGIN | C→S |
| 0x0002 | S2C_LOGIN | S→C |
| 0x0003 | C2S_CHAT | C→S |
| 0x0004 | S2C_CHAT | S→C |
| 0x0005 | C2S_HEARTBEAT | C→S |
| 0x0006 | S2C_HEARTBEAT | S→C |
| 0x0007 | C2S_JOIN_ROOM | C→S |
| 0x0008 | S2C_JOIN_ROOM | S→C |
| 0x00FF | S2C_ERROR | S→C |
| 0x0081 | C2S_PLAYER_MOVE | C→S |
| 0x0082 | S2C_PLAYER_MOVE | S→C |

**帧同步（`frame_sync.h`，前缀 0x01xx）**

| 命令字 | 名称 |
|--------|------|
| 0x0101 | C2S_FRAME_INPUT |
| 0x0102 | C2S_FRAME_SYNC_REQ |
| 0x0103 | S2C_FRAME_SYNC |
| 0x0104 | S2C_FRAME_STATE |
| 0x0105 | S2C_FRAME_SNAPSHOT |

**状态同步（`state_sync.h`，前缀 0x02xx）**

| 命令字 | 名称 |
|--------|------|
| 0x0201 | C2S_STATE_UPDATE |
| 0x0202 | C2S_STATE_QUERY |
| 0x0203 | C2S_STATE_SUBSCRIBE |
| 0x0204 | C2S_STATE_UNSUBSCRIBE |
| 0x0205 | S2C_STATE_UPDATE |
| 0x0206 | S2C_STATE_DIFF |
| 0x0207 | S2C_STATE_SNAPSHOT |

---

## 构建选项

| CMake 选项 | 默认值 | 说明 |
|------------|--------|------|
| `CHWELL_BUILD_EXAMPLES` | `ON` | 编译示例程序 |
| `CHWELL_BUILD_TESTS` | `ON` | 编译单元测试（GoogleTest 自动 FetchContent）|
| `CHWELL_USE_YAML` | `ON` | yaml-cpp（存储配置解析）|
| `CHWELL_USE_PROTOBUF` | `ON` | Protobuf（帧编解码示例）|
| `CHWELL_USE_MYSQL` | `OFF` | MySQL 存储后端 |
| `CHWELL_USE_MONGODB` | `OFF` | MongoDB 存储后端 |
| `CHWELL_USE_OPENSSL` | `OFF` | OpenSSL TLS / WebSocket SHA-1 握手 |

**最小化构建（无可选依赖）：**

```bash
cmake -B build \
  -DCHWELL_BUILD_TESTS=OFF \
  -DCHWELL_USE_YAML=OFF \
  -DCHWELL_USE_PROTOBUF=OFF
cmake --build build -j$(nproc)
```

**依赖安装（Ubuntu/Debian）：**

```bash
# 必须
sudo apt install build-essential cmake

# 推荐
sudo apt install libyaml-cpp-dev libssl-dev

# 可选
sudo apt install libmysqlclient-dev libmongoc-dev
```

---

## 测试

### 单元测试

```bash
cd build

# 运行全部测试
./chwell_core_tests

# 排除需要本地 Redis 服务的用例
./chwell_core_tests --gtest_filter="-*Redis*"

# 仅 benchmark 相关
./chwell_core_tests --gtest_filter="BenchmarkTest.*"
```

### 集成测试

```bash
./chwell_integration_tests
```

### echo QPS 压测（ChwellBenchmark）

```bash
# 先启动 echo 服务
./example_echo_server

# 然后运行压测
cd ChwellBenchmark/build
./echo_qps_bench --host 127.0.0.1 --port 9000 --connections 10000 --duration 30
```

### 测试覆盖

| 测试文件 | 覆盖模块 |
|----------|----------|
| `test_protocol_parser.cpp` | 协议序列化 / 粘包解析 |
| `test_protocol_router.cpp` | 命令字路由 |
| `test_session_manager.cpp` | 会话管理（登录/登出/房间绑定）|
| `test_timer_wheel.cpp` | 时间轮定时器（一次性/重复/取消/排序）|
| `test_aoi.cpp` | CrossListAoi |
| `test_event_bus.cpp` | 事件总线（订阅/发布/优先级/线程安全）|
| `test_object_pool.cpp` | ObjectPool / BufferPool |
| `test_task_queue.cpp` | TaskQueue（延时/重复/取消）|
| `test_sync.cpp` | 帧同步 / 状态同步（房间 / 输入 / 快照）|
| `test_discovery_loadbalance.cpp` | 服务发现 / 负载均衡 |
| `test_consistent_hash.cpp` | 一致性哈希 |
| `test_circuitbreaker.cpp` | 熔断器（计数 / 失败率 / 混合 / 半开）|
| `test_ratelimit.cpp` | 限流器（令牌桶 / 漏桶 / 固定窗口）|
| `test_prometheus_metrics.cpp` | Prometheus 指标导出 |
| `test_benchmark.cpp` | Benchmark 框架 + 协议层微基准 |
| `test_rpc.cpp` | RPC 并发 / 超时 / 熔断集成 |
| `test_slg.cpp` | SLG 地图 / 战斗系统 |
| `test_game_components.cpp` | 游戏组件编解码 |
| `test_player_move.cpp` | 玩家移动同步 |
| `test_udp_socket.cpp` | UDP Socket 创建 / 绑定 / 发送接收 |
| `test_orm_repository.cpp` | ORM 仓储 CRUD |
| `test_storage.cpp` | Document 序列化、MemoryStorage TTL、批量操作、StorageFactory、StorageComponent nullptr guard、AsyncStorageAdapter（Future/Callback/并发）|
| `test_redis_client.cpp` | Redis 客户端（需本地 Redis）|
| `test_gateway_multinode.cpp` | 多节点注册 / 服务发现 / 分布式锁 |

---

## 配置文件

### `config/storage.yaml`

```yaml
storage:
  type: memory       # memory | mysql | mongodb
  mysql:
    host: 127.0.0.1
    port: 3306
    database: chwell
    user: root
    password: ""
  mongodb:
    uri: mongodb://127.0.0.1:27017
    database: chwell
```

### `config/cluster.yaml`

```yaml
nodes:
  - id: game_server_1
    type: game
    host: 127.0.0.1
    port: 9000
  - id: game_server_2
    type: game
    host: 127.0.0.1
    port: 9001
```

### `config/server.conf`

```ini
listen_port = 9000
worker_threads = 4
```

---

## 目录结构

```
ChwellCore/
├── include/chwell/               # 对外头文件
│   ├── core/                     # config · endian · logger · thread_pool · timer_wheel
│   ├── net/
│   │   ├── posix_io.h            # POSIX socket/poll 封装
│   │   ├── tcp_server.h / tcp_connection.h
│   │   ├── udp_server.h / udp_socket.h / socket_base.h
│   │   ├── ws_server.h / ws_connection.h   # 含 send_binary
│   │   ├── http_server.h
│   │   ├── connection_pool.h
│   │   ├── connection_adapter.h  # IConnection 统一适配器（主接口）
│   │   ├── connection.h          # IBaseConnection（轻量基类）
│   │   ├── net_interface.h       # INetConnection / IServer
│   │   └── tls.h
│   ├── protocol/                 # message.h · parser.h
│   ├── codec/                    # ProtobufCodec · JsonCodec
│   ├── service/                  # Service · Component · ProtocolRouter · SessionManager
│   ├── game/                     # game_components.h · player_move.h
│   ├── sync/                     # frame_sync.h · state_sync.h
│   ├── task/                     # task_queue.h (TaskQueue · DelayedTaskQueue)
│   ├── pool/                     # object_pool.h (ObjectPool<T> · BufferPool)
│   ├── event/                    # event_bus.h
│   ├── aoi/                      # aoi.h (GridAoi · CrossListAoi)
│   ├── slg/                      # map.h · battle.h
│   ├── storage/                  # 接口 · 工厂 · 各后端 · ORM(entity/document/repository)
│   ├── cluster/                  # node.h · node_registry.h
│   ├── rpc/                      # rpc_client.h · rpc_server.h
│   ├── gateway/                  # gateway_forwarder.h
│   ├── redis/                    # redis_client.h · distributed_lock.h
│   ├── discovery/                # service_discovery.h
│   ├── loadbalance/              # load_balancer.h · consistent_hash.h
│   ├── circuitbreaker/           # circuit_breaker.h
│   ├── ratelimit/                # rate_limiter.h
│   ├── metrics/                  # prometheus_metrics.h
│   └── benchmark/                # benchmark.h
├── src/                          # 与 include/ 镜像的实现文件
├── tests/                        # GoogleTest 单元测试（212 个用例）
├── integration_tests/            # 集成测试
├── examples/
│   ├── echo_server.cpp
│   ├── protocol_server.cpp · protocol_stress_client.cpp
│   ├── http_server.cpp
│   ├── gateway_server.cpp
│   ├── game_server.cpp · game_gateway_server.cpp
│   ├── game_commands.h           # H5 Demo 命令号常量
│   ├── game_components_demo.cpp · game_ws_components_demo.cpp
│   ├── sync_demo.cpp · new_modules_demo.cpp
│   ├── storage_example.cpp · orm_example.cpp
│   ├── proto_frame_server.cpp · proto_frame_client.cpp
│   ├── json_frame_client.cpp
│   ├── ws_tcp_bridge.cpp         # WS↔TCP 桥（需 OpenSSL）
│   ├── game_ws_server.cpp · game_ws_server_simple.cpp · game_ws_server_json.cpp
│   └── h5_game/
│       ├── index.html            # H5 对战前端（纯静态）
│       └── README.md
├── proto/
│   └── game.proto
├── config/
│   ├── cluster.yaml · server.conf · storage.yaml
├── CMakeLists.txt
└── README.md
```

---

## 路线图

### 已完成

- 核心网络层（TCP / UDP / WebSocket / HTTP / TLS）
- 自定义二进制协议 + 流式粘包解析
- 组件化游戏服务层（登录 / 聊天 / 房间 / 心跳 / 移动）
- 帧同步 / 状态同步
- 时间轮定时器 / 线程池 / 对象池 / 任务队列
- 存储抽象（内存 / MySQL / MongoDB）+ ORM
- 集群节点注册 + RPC + 网关转发
- 服务发现 + 负载均衡 + 一致性哈希
- 熔断器（计数 / 失败率 / 混合策略，`execute_with_result` 正确计数）+ 限流器
- Prometheus 指标
- Redis 客户端 + 分布式锁
- AOI（格子 / 十字链表）+ SLG 模块
- Benchmark 框架（含 tcp / memory / loadbalance / protocol 基准全实现）
- H5 对战 Demo（完整前后端）
- 存储模块全面整治（MySQL 大值修复、MySQL/MongoDB keys() 修复、MongoDB 全局 init 修复）
- 异步存储接口（`AsyncStorageInterface` + `AsyncStorageAdapter`，Future + Callback 两套 API）
- 单元测试全覆盖（258 个测试用例）

### 规划中

- 结构化日志（spdlog 集成选项）
- 更多游戏组件（好友、公会、排行榜）
- 热更新支持
- 更多集成测试场景

---

## 许可证

本项目采用 MIT 许可证，详见 [LICENSE](LICENSE)。
