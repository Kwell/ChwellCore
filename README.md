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

- 🚀 **高性能网络层** - 基于 ASIO 的非阻塞 TCP/HTTP 服务
- 📦 **协议编解码** - 长度前缀帧、Protobuf、JSON 支持
- 🎮 **游戏模块** - AOI、寻路、战斗系统、SLG 地图
- 💾 **存储抽象** - 内存/MySQL/MongoDB 可切换
- 🔧 **集群支持** - 节点注册、RPC 调用
- 📊 **可靠性** - 心跳、限流、监控指标

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

# 游戏服务器
./example_game_server
```

## 核心模块

### 网络层 (`chwell/net`)
- `TcpServer` / `TcpConnection` - TCP 服务
- `HttpServer` - HTTP 服务
- `ConnectionPool` - 连接池

### 编解码 (`chwell/codec`)
- `LengthHeaderCodec` - 长度前缀帧（推荐）
- `ProtobufCodec` - Protobuf 序列化
- `JsonCodec` - JSON 序列化

### 协议路由 (`chwell/protocol`)
- `ProtocolRouter` - 消息类型路由
- `ProtocolParser` - 协议解析

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
- `Metrics` - 监控指标

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
