# ChwellCore - 游戏后端核心框架

一个模块化、高性能的 C++ 游戏服务器框架，专为 SLG/MMO 等中大型游戏设计。

## 技术选型

### 后端（本框架）

| 模块 | 技术方案 | 选择理由 |
|------|----------|----------|
| **网络层** | ASIO (Standalone) | 业界标准异步 I/O 库，性能优异，跨平台，被广泛使用（如 MongoDB 驱动） |
| **序列化** | Protobuf | Google 出品，高效二进制序列化，跨语言支持，游戏行业首选 |
| **JSON** | nlohmann/json | 现代 C++ JSON 库，API 友好，Header-only，GitHub 40k+ stars |
| **数据库** | MySQL | 最成熟的关系型数据库，游戏行业标配 |
| **缓存** | Redis | 高性能内存数据库，排行榜/会话/缓存首选 |
| **文档库** | MongoDB | 灵活文档存储，适合玩家数据/日志/配置 |
| **日志** | 自实现 (可换 spdlog) | 简单线程安全日志，生产环境可替换为 spdlog |
| **配置** | YAML-cpp | 人类可读配置格式，比 JSON/INI 更灵活 |
| **构建** | CMake | 跨平台构建系统，C++ 项目标配 |
| **测试** | Google Test | 业界标准单元测试框架 |

### 前端（H5 客户端推荐）

| 模块 | 技术方案 | 选择理由 |
|------|----------|----------|
| **游戏引擎** | Cocos Creator | 国产引擎，H5 游戏首选，可视化编辑器，TypeScript 支持，社区活跃 |
| **备选** | Phaser 3 | 轻量级 2D 框架，适合中小型项目，GitHub 36k+ stars |
| **备选** | PixiJS | 高性能 2D 渲染引擎，灵活但需要自己搭建游戏框架 |
| **通信** | WebSocket | 浏览器原生支持，实时双向通信 |
| **状态管理** | Redux / Zustand | 前端状态管理标准方案 |
| **UI 框架** | React / Vue 3 | 现代前端框架，组件化开发 |

### 为什么这样选？

1. **ASIO vs libuv vs 自写**
   - ASIO 是 C++ 标准库网络提案的基础，未来可能进入标准
   - 性能足够支撑万人在线，代码质量高

2. **Protobuf vs FlatBuffers vs JSON**
   - Protobuf 生态最成熟，工具链完善
   - FlatBuffers 更快但开发体验差
   - JSON 仅用于调试和简单协议

3. **MySQL vs PostgreSQL**
   - MySQL 在游戏行业应用更广
   - 运维经验丰富的人才更多

4. **Cocos Creator vs Phaser vs Unity**
   - Cocos: 国产引擎，中文文档全，H5 性能优化好
   - Phaser: 轻量但功能有限，适合小项目
   - Unity: 功能最强但 H5 包体大，加载慢

## 特性

- 🚀 **高性能网络层** - 基于 ASIO 的非阻塞 TCP/HTTP/WebSocket 服务
- 📦 **协议编解码** - 长度前缀帧、Protobuf、JSON 支持
- 🎮 **游戏组件系统** - 登录、聊天、房间、心跳、玩家移动等模块化组件
- 🔄 **同步系统** - 帧同步、状态同步，支持实时游戏
- 🎯 **AOI 系统** - 格子 AOI、十字链表 AOI
- 🗺️ **SLG 模块** - 地图管理、战斗系统
- 💾 **存储抽象** - 内存/MySQL/MongoDB 可切换
- 🌐 **集群支持** - 节点注册、RPC 调用
- 📊 **可靠性** - 心跳、限流、监控指标
- 🔌 **连接适配器** - TCP 和 WebSocket 统一接口

## 快速开始

### 依赖

- CMake 3.14+
- C++17 编译器
- 可选: MySQL, MongoDB, OpenSSL

### 构建

```bash
mkdir build && cd build
cmake .. -DCHWELL_BUILD_TESTS=ON
make -j$(nproc)
```

### 运行示例

```bash
# Echo 服务器
./example_echo_server

# 网关服务器
./example_gateway_server

# 游戏服务器（基础）
./example_game_server

# 游戏组件服务器（TCP）
./example_game_components_demo

# WebSocket + 游戏组件服务器
./example_game_ws_components_demo

# 帧同步 + 状态同步服务器
./example_sync_demo

# Protobuf 示例
./example_proto_frame_server
```

## 核心模块

### 网络层 (`chwell/net`)
- `TcpServer` / `TcpConnection` - TCP 服务
- `WsServer` / `WsConnection` - WebSocket 服务
- `HttpServer` - HTTP 服务
- `ConnectionPool` - 连接池
- `ConnectionAdapter` - TCP/WebSocket 连接适配器

### 服务层 (`chwell/service`)
- `Service` - 服务容器
- `Component` - 组件基类
- `ProtocolRouterComponent` - 协议路由
- `SessionManager` - 会话管理

### 游戏组件 (`chwell/game`)
- `LoginComponent` - 登录组件
- `ChatComponent` - 聊天组件
- `RoomComponent` - 房间组件
- `HeartbeatComponent` - 心跳组件
- `PlayerMoveComponent` - 玩家移动组件

### 编解码 (`chwell/codec`)
- `LengthHeaderCodec` - 长度前缀帧（推荐）
- `ProtobufCodec` - Protobuf 序列化
- `JsonCodec` - JSON 序列化

### 协议 (`chwell/protocol`)
- `ProtocolRouter` - 消息类型路由
- `ProtocolParser` - 协议解析
- `Message` - 协议消息结构

### 同步系统 (`chwell/sync`)
- `FrameSyncComponent` - 帧同步组件
- `FrameSyncRoom` - 帧同步房间
- `StateSyncComponent` - 状态同步组件
- `StateSyncRoom` - 状态同步房间

### 任务调度 (`chwell/task`)
- `TaskQueue` - 线程池任务队列
- `DelayedTaskQueue` - 延时任务
- `TimerWheel` - 高效定时器

### 对象池 (`chwell/pool`)
- `ObjectPool<T>` - 通用对象池
- `BufferPool` - 缓冲区池
- `GlobalBufferPool` - 全局缓冲区管理

### AOI 系统 (`chwell/aoi`)
- `GridAoi` - 格子 AOI
- `CrossListAoi` - 十字链表 AOI

### SLG 模块 (`chwell/slg`)
- `SlgMapManager` - 地图管理
- `BattleSystem` - 战斗系统
- 支持地形生成、资源点、城池、部队

### 存储层 (`chwell/storage`)
- `StorageInterface` - 统一存储接口
- `MemoryStorage` - 内存存储
- `MysqlStorage` - MySQL 存储
- `MongodbStorage` - MongoDB 存储
- `Repository<T>` - ORM 仓储

### 集群与 RPC (`chwell/cluster`, `chwell/rpc`)
- `NodeRegistry` - 节点注册表
- `RpcServer` / `RpcClient` - RPC 调用

### 网关 (`chwell/gateway`)
- `GatewayForwarderComponent` - 消息转发
- 支持客户端与后端服务映射

### Redis 客户端 (`chwell/redis`)
- `RedisClient` - Redis 操作封装
- 内存 Mock 实现（测试用）

### 事件总线 (`chwell/event`)
- `EventBus` - 发布/订阅事件系统

### 可靠性 (`chwell/reliability`)
- `HeartbeatManager` - 心跳管理
- `RateLimiter` - 令牌桶限流
- `CircuitBreaker` - 熔断器
- `Metrics` - 监控指标
- `ServiceDiscovery` - 服务发现
- `LoadBalancer` - 负载均衡

## 游戏协议

### 基础协议

| 命令字 | 类型 | 说明 |
|--------|------|------|
| 0x0001 | C2S_LOGIN | 登录请求 |
| 0x0002 | S2C_LOGIN | 登录响应 |
| 0x0003 | C2S_CHAT | 聊天请求 |
| 0x0004 | S2C_CHAT | 聊天响应 |
| 0x0005 | C2S_HEARTBEAT | 心跳请求 |
| 0x0006 | S2C_HEARTBEAT | 心跳响应 |
| 0x0007 | C2S_JOIN_ROOM | 加入房间请求 |
| 0x0008 | S2C_JOIN_ROOM | 加入房间响应 |
| 0x00FF | S2C_ERROR | 错误响应 |

### 扩展协议

| 命令字 | 类型 | 说明 |
|--------|------|------|
| 0x0081 | C2S_PLAYER_MOVE | 玩家移动请求 |
| 0x0082 | S2C_PLAYER_MOVE | 玩家移动响应 |
| 0x0083 | S2C_PLAYER_POS | 玩家位置广播 |

### 帧同步协议

| 命令字 | 类型 | 说明 |
|--------|------|------|
| 0x0101 | C2S_FRAME_INPUT | 帧输入 |
| 0x0102 | C2S_FRAME_SYNC_REQ | 帧同步请求 |
| 0x0103 | S2C_FRAME_SYNC | 帧同步响应 |
| 0x0104 | S2C_FRAME_STATE | 帧状态 |
| 0x0105 | S2C_FRAME_SNAPSHOT | 帧快照 |

### 状态同步协议

| 命令字 | 类型 | 说明 |
|--------|------|------|
| 0x0201 | C2S_STATE_UPDATE | 状态更新 |
| 0x0202 | C2S_STATE_QUERY | 状态查询 |
| 0x0203 | C2S_STATE_SUBSCRIBE | 状态订阅 |
| 0x0204 | C2S_STATE_UNSUBSCRIBE | 取消订阅 |
| 0x0205 | S2C_STATE_UPDATE | 状态更新响应 |
| 0x0206 | S2C_STATE_DIFF | 状态差异 |
| 0x0207 | S2C_STATE_SNAPSHOT | 状态快照 |
| 0x02FF | S2C_STATE_ERROR | 状态错误 |

## 使用示例

### 创建 TCP 服务

```cpp
#include "chwell/net/tcp_server.h"
#include "chwell/codec/length_header_codec.h"

net::TcpServer server(io_context, 9000);
server.set_on_message([](const std::vector<char>& data) {
    // 处理消息
});
server.start();
```

### 使用游戏组件

```cpp
#include "chwell/service/service.h"
#include "chwell/game/game_components.h"

service::Service svc(9000, 4);

svc.add_component<service::SessionManager>();
svc.add_component<service::ProtocolRouterComponent>();
svc.add_component<game::LoginComponent>();
svc.add_component<game::ChatComponent>();
svc.add_component<game::RoomComponent>();
svc.add_component<game::HeartbeatComponent>();
svc.add_component<game::PlayerMoveComponent>();

svc.start();
```

### 使用对象池

```cpp
pool::ObjectPool<Bullet> bullet_pool;
auto bullet = bullet_pool.acquire();
// 使用后自动归还
```

### 使用任务队列

```cpp
task::TaskQueue queue(4);  // 4 个工作线程
queue.start();

queue.submit_void([]() {
    // 异步任务
});

queue.stop();
```

### 使用 AOI

```cpp
aoi::GridAoi aoi(1000, 1000, 50);  // 1000x1000 地图，50 格子大小
aoi.add_entity(entity);
auto nearby = aoi.get_entities_in_view(x, y);
```

### 使用存储

```cpp
// 从配置创建
auto store = storage::StorageFactory::create_from_yaml("config/storage.yaml");
store->put("player:123", player_data);
auto result = store->get("player:123");
```

## 配置文件

### storage.yaml

```yaml
storage:
  type: memory  # memory | mysql | mongodb
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

## 构建选项

| 选项 | 说明 |
|------|------|
| `CHWELL_BUILD_TESTS` | 构建单元测试 |
| `CHWELL_USE_MYSQL` | 启用 MySQL 支持 |
| `CHWELL_USE_MONGODB` | 启用 MongoDB 支持 |
| `CHWELL_USE_YAML` | 启用 YAML 配置支持 |
| `CHWELL_USE_PROTOBUF` | 启用 Protobuf 支持 |

## 测试

```bash
cd build
./chwell_core_tests
```

## 目录结构

```
ChwellCore/
├── include/chwell/     # 头文件
│   ├── core/           # 核心组件（日志、定时器）
│   ├── net/            # 网络层
│   ├── codec/          # 编解码
│   ├── protocol/       # 协议路由
│   ├── task/           # 任务调度
│   ├── pool/           # 对象池
│   ├── aoi/            # AOI
│   ├── slg/            # SLG 模块
│   ├── storage/        # 存储层
│   ├── cluster/        # 集群
│   ├── rpc/            # RPC
│   ├── gateway/        # 网关
│   ├── redis/          # Redis
│   ├── event/          # 事件总线
│   └── reliability/    # 可靠性
├── src/                # 实现文件
├── tests/              # 单元测试
├── examples/           # 示例程序
├── proto/              # Protobuf 定义
└── config/             # 配置文件
```

## 许可证

MIT License
