# Chwell H5 对战小游戏 Demo

基于 ChwellCore 框架实现的 **完整对战游戏 Demo**，覆盖登录鉴权 → 游戏大厅 → 开房间 → 回合制对战的端到端流程。前端为单个静态 HTML 文件，后端由三个可执行进程组成，前后端通过自定义二进制协议通信。

---

## 目录

1. [架构概览](#架构概览)
2. [目录结构](#目录结构)
3. [依赖与环境要求](#依赖与环境要求)
4. [编译](#编译)
5. [运行方式](#运行方式)
   - [启动后端](#启动后端)
   - [配置文件与环境变量](#配置文件与环境变量)
   - [启动前端](#启动前端)
   - [体验对局](#体验对局)
6. [前后端分离部署](#前后端分离部署)
7. [协议规范](#协议规范)
   - [帧格式](#帧格式)
   - [命令号一览](#命令号一览)
   - [各命令详解](#各命令详解)
   - [JSON 结构说明](#json-结构说明)
8. [游戏逻辑说明](#游戏逻辑说明)
9. [常见问题排查](#常见问题排查)
10. [接入自定义客户端](#接入自定义客户端)
11. [已知限制](#已知限制)

---

## 架构概览

```
浏览器（index.html）
      │  WebSocket（二进制帧）
      ▼
ws_tcp_bridge          :9080   透明 WS↔TCP 转发（WebSocket 握手 + 帧拆包）
      │  TCP（原始二进制协议）
      ▼
example_game_gateway_server    :9001   网关层（LOGIN/LOGOUT/HEARTBEAT 本地处理，其余转发）
      │  TCP（原始二进制协议）
      ▼
example_game_server            :9000   游戏逻辑层（房间、对战状态机）
```

**职责分工**

| 进程 | 本地处理的命令 | 说明 |
|------|----------------|------|
| `ws_tcp_bridge` | — | 纯 WebSocket ↔ TCP 帧转发，不解析业务逻辑 |
| `example_game_gateway_server` | LOGIN / LOGOUT / HEARTBEAT | 会话鉴权，其余命令直接转发到游戏服 |
| `example_game_server` | LIST_ROOMS / CREATE_ROOM / JOIN_ROOM / LEAVE_ROOM / START_GAME / GAME_ACTION | 房间管理与对战状态机 |

---

## 目录结构

```
ChwellCore/
├── examples/
│   ├── h5_game/
│   │   ├── index.html          # 前端：纯静态单页面，无外部依赖
│   │   └── README.md           # 本文档
│   ├── game_server.cpp         # 游戏逻辑服源码
│   ├── game_gateway_server.cpp # 游戏网关源码
│   ├── ws_tcp_bridge.cpp       # WebSocket↔TCP 桥接源码
│   └── game_commands.h         # 命令号常量定义（前后端共享）
├── config/
│   └── server.conf             # 游戏服默认配置（端口 9000，线程 4）
└── gateway.conf                # 网关默认配置（端口 9001，后端 127.0.0.1:9000）
```

---

## 依赖与环境要求

| 依赖 | 版本要求 | 用途 | 安装（Ubuntu/Debian） |
|------|----------|------|-----------------------|
| CMake | ≥ 3.11 | 构建系统 | `apt install cmake` |
| GCC / Clang | 支持 C++17 | 编译器 | `apt install build-essential` |
| **OpenSSL** | ≥ 1.1 | WebSocket 握手（SHA-1）| `apt install libssl-dev` |
| pthread | — | 线程（Linux 内置）| — |

> **注意**：`ws_tcp_bridge` 和 `example_game_ws_server` 依赖 OpenSSL。其他 demo 可选。

可选依赖（不影响 H5 游戏 Demo）：

| 依赖 | CMake 选项 | 说明 |
|------|------------|------|
| yaml-cpp ≥ 0.6 | `CHWELL_USE_YAML=ON`（默认）| 存储配置文件解析 |
| protobuf | `CHWELL_USE_PROTOBUF=ON`（默认）| 帧协议示例 |
| libmysqlclient | `CHWELL_USE_MYSQL=ON` | MySQL 存储后端 |
| libmongoc | `CHWELL_USE_MONGODB=ON` | MongoDB 存储后端 |

---

## 编译

```bash
# 在仓库根目录执行
mkdir -p build && cd build

# 最小编译（仅构建示例，关闭可选依赖）
cmake .. \
  -DCHWELL_BUILD_EXAMPLES=ON \
  -DCHWELL_BUILD_TESTS=OFF \
  -DCHWELL_USE_YAML=OFF \
  -DCHWELL_USE_PROTOBUF=OFF

cmake --build . -j$(nproc)
```

编译成功后，以下可执行文件位于 `build/` 目录：

```
build/
├── example_game_server
├── example_game_gateway_server
└── ws_tcp_bridge
```

---

## 运行方式

### 启动后端

建议在 `build/` 目录下开 **3 个终端**分别启动，启动顺序：游戏服 → 网关 → 桥接。

**终端 1 — 游戏逻辑服（端口 9000）**

```bash
cd build
./example_game_server
```

输出示例：
```
[INFO] Starting Game Server...
[INFO] Service started
[INFO] Game Server running on port 9000
Press ENTER to exit...
```

**终端 2 — 游戏网关（端口 9001）**

```bash
cd build
./example_game_gateway_server
```

输出示例：
```
[INFO] Starting Game Gateway Server...
[INFO] Game Gateway on port 9001, backend 127.0.0.1:9000
Press ENTER to exit...
```

**终端 3 — WebSocket 桥接（端口 9080）**

```bash
cd build
./ws_tcp_bridge
# 或指定监听端口：
./ws_tcp_bridge 9080
```

输出示例：
```
WS-TCP Bridge: ws://0.0.0.0:9080 -> 127.0.0.1:9001
```

---

### 配置文件与环境变量

#### 游戏逻辑服（`server.conf`）

配置文件从**当前工作目录**读取，找不到时使用内置默认值：

```ini
listen_port = 9000      # 监听端口
worker_threads = 4      # IO 线程池大小
```

#### 游戏网关（`gateway.conf` 或环境变量）

配置文件或环境变量，**环境变量优先级高于配置文件**：

| 环境变量 | 默认值 | 说明 |
|----------|--------|------|
| `GATEWAY_PORT` | `9001` | 网关监听端口 |
| `BACKEND_HOST` | `127.0.0.1` | 游戏服地址 |
| `BACKEND_PORT` | `9000` | 游戏服端口 |

示例——网关和游戏服部署在不同机器：

```bash
BACKEND_HOST=10.0.0.2 BACKEND_PORT=9000 ./example_game_gateway_server
```

#### WebSocket 桥接（`ws_tcp_bridge`）

| 方式 | 说明 |
|------|------|
| 第一个命令行参数 | 覆盖监听端口，如 `./ws_tcp_bridge 9080` |
| `GATEWAY_HOST` | 覆盖转发目标地址（默认 `127.0.0.1`）|
| `GATEWAY_PORT` | 覆盖转发目标端口（默认 `9001`）|

示例——桥接转发到远端网关：

```bash
GATEWAY_HOST=10.0.0.1 GATEWAY_PORT=9001 ./ws_tcp_bridge 9080
```

---

### 启动前端

在任意 HTTP 服务下暴露 `index.html`：

```bash
cd examples/h5_game
python3 -m http.server 8080
```

浏览器打开 `http://localhost:8080`。

页面内「服务端 WebSocket 地址」默认为 `ws://localhost:9080`；首次填写后会记住。  
也可通过 URL 参数指定：

```
http://localhost:8080?ws=ws://localhost:9080
http://localhost:8080?ws=wss://api.example.com      # TLS 部署
```

---

### 体验对局

1. **打开两个浏览器标签**（或一个无痕 + 一个普通），确保后端 3 个进程均已运行。
2. 两个标签分别输入**不同的昵称**，点击「连接并登录」，状态栏显示「已连接，正在登录…」后自动进入大厅。
3. 一个玩家点击「创建房间」，另一个点「刷新列表」后在列表中点「加入」。
4. 两人都在房间后，**房主**点「开始游戏」（按钮在 1 人时置灰）。
5. 进入对战：  
   - `turn` 字段指示当前操作方（座位 0 = 先创建者，座位 1 = 后加入者）。  
   - **攻击**：对方 HP -10；**防御**：本回合不扣血，双方均切换回合计数。  
   - HP 先归零的一方判负，显示胜负结果后可点「返回房间」再战。

---

## 前后端分离部署

前端 `index.html` 为纯静态文件，可托管在任意位置（CDN、对象存储、Nginx 等），与后端完全解耦。

```
前端                          后端
────────────────────          ────────────────────────────────────
CDN / Nginx / Vercel          云服务器 / 内网集群
index.html                    ws_tcp_bridge  ←→  gateway  ←→  game_server
      │
      │ WebSocket（用户手动填写或 ?ws= 参数）
      ▼
      wss://api.example.com   ← 公网暴露（需配置 TLS 反向代理）
```

**TLS 反向代理示例（Nginx）**：

```nginx
server {
    listen 443 ssl;
    server_name api.example.com;

    ssl_certificate     /etc/ssl/certs/api.example.com.crt;
    ssl_certificate_key /etc/ssl/private/api.example.com.key;

    location /ws {
        proxy_pass http://127.0.0.1:9080;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "Upgrade";
        proxy_read_timeout 3600s;
    }
}
```

页面填写地址：`wss://api.example.com/ws`

---

## 协议规范

### 帧格式

所有消息均通过 WebSocket **二进制帧**传输，载荷（payload）布局如下：

```
┌─────────┬──────────┬────────────────────┐
│  cmd    │   len    │       body         │
│ 2 字节  │ 2 字节   │    len 字节        │
│ 大端序  │ 大端序   │    UTF-8 或二进制  │
└─────────┴──────────┴────────────────────┘
```

- `cmd`：命令号（uint16，大端）
- `len`：body 字节长度（uint16，大端），最大 65535 字节
- `body`：各命令的有效载荷，格式见下方说明

**JavaScript 封包示例**：

```javascript
function writeMsg(cmd, body) {
  const bodyBytes = typeof body === 'string'
    ? new TextEncoder().encode(body)
    : body;                              // 已是 Uint8Array/ArrayBuffer
  const buf = new ArrayBuffer(4 + bodyBytes.length);
  const view = new DataView(buf);
  view.setUint16(0, cmd, false);         // 大端
  view.setUint16(2, bodyBytes.length, false);
  new Uint8Array(buf).set(bodyBytes, 4);
  return buf;
}
```

**JavaScript 拆包示例**：

```javascript
let recvBuf = [];
ws.onmessage = (ev) => {
  recvBuf.push(...new Uint8Array(ev.data));
  while (recvBuf.length >= 4) {
    const view = new DataView(new Uint8Array(recvBuf).buffer);
    const cmd = view.getUint16(0, false);
    const len = view.getUint16(2, false);
    if (recvBuf.length < 4 + len) break;
    const bodyBytes = new Uint8Array(recvBuf.slice(4, 4 + len));
    const body = new TextDecoder().decode(bodyBytes);
    recvBuf = recvBuf.slice(4 + len);
    onPacket(cmd, body);
  }
};
```

---

### 命令号一览

| 命令 | 值（十进制）| 值（十六进制）| 方向 |
|------|:-----------:|:-------------:|------|
| HEARTBEAT   | 3  | 0x0003 | C↔S |
| LOGIN       | 10 | 0x000A | C→S / S→C |
| LOGOUT      | 11 | 0x000B | C→S / S→C |
| LIST_ROOMS  | 20 | 0x0014 | C→S / S→C |
| CREATE_ROOM | 21 | 0x0015 | C→S / S→C |
| JOIN_ROOM   | 22 | 0x0016 | C→S / S→C |
| LEAVE_ROOM  | 23 | 0x0017 | C→S / S→C |
| ROOM_INFO   | 24 | 0x0018 | S→C（服务端主动推送）|
| START_GAME  | 25 | 0x0019 | C→S / S→C |
| GAME_STATE  | 26 | 0x001A | S→C（服务端主动推送）|
| GAME_ACTION | 27 | 0x001B | C→S / S→C |

---

### 各命令详解

#### HEARTBEAT（3）

用于保持长连接，由**网关层**直接处理，不转发到游戏服。

| 方向 | body 格式 | 说明 |
|------|-----------|------|
| C→S | 8 字节，小端 int64 | 客户端当前时间戳（毫秒），服务端不使用此值 |
| S→C | UTF-8 字符串 `"pong"` | 固定回显 |

> 前端每 15 秒发送一次。

---

#### LOGIN（10）

由**网关层**处理，完成会话绑定后直接响应，不转发到游戏服。

| 方向 | body 格式 | 说明 |
|------|-----------|------|
| C→S | UTF-8 字符串 | 玩家昵称（不可为空，最大 32 字符）|
| S→C 成功 | `"login ok: <昵称>"` | 登录成功 |
| S→C 失败 | `"login failed: empty player_id"` | 昵称为空 |

---

#### LOGOUT（11）

由**网关层**处理。

| 方向 | body 格式 | 说明 |
|------|-----------|------|
| C→S | 空 | — |
| S→C 成功 | `"logout ok: <昵称>"` | 登出成功 |
| S→C 失败 | `"not logged in"` | 未登录 |

---

#### LIST_ROOMS（20）

查询当前**可加入**的房间（`state=0` 且人数 < 2）。

| 方向 | body 格式 | 说明 |
|------|-----------|------|
| C→S | 空 | — |
| S→C | JSON 数组 | 见 [ROOM_INFO JSON](#room_info-json) |

响应示例：

```json
[
  {"id":"r1234567890","name":"快来玩","players":1,"state":0},
  {"id":"r9876543210","name":"room","players":1,"state":0}
]
```

---

#### CREATE_ROOM（21）

创建房间并自动加入（房主为座位 0）。

| 方向 | body 格式 | 说明 |
|------|-----------|------|
| C→S | UTF-8 字符串 | 房间名（可为空，空时自动命名为 `"room"`）|
| S→C 成功 | 房间 ID 字符串（如 `"r1234567890123"`）| 创建成功 |
| S→C 失败 | `"err:login"` / `"err:in_room"` | 见[错误码](#错误码) |

创建成功后服务端同时推送一条 ROOM_INFO。

---

#### JOIN_ROOM（22）

加入指定房间（成为座位 1）。

| 方向 | body 格式 | 说明 |
|------|-----------|------|
| C→S | 房间 ID（UTF-8 字符串）| — |
| S→C 成功 | `"ok"` | 加入成功 |
| S→C 失败 | `"err:login"` / `"err:no_room"` / `"err:full"` / `"err:in_room"` | 见[错误码](#错误码) |

加入成功后服务端同时向**房间内所有人**推送 ROOM_INFO。

---

#### LEAVE_ROOM（23）

主动离开房间。离开后若房间内只剩 1 人且游戏进行中，另一方收到 `result="opponent_left"` 的 GAME_STATE。

| 方向 | body 格式 | 说明 |
|------|-----------|------|
| C→S | 空 | — |
| S→C | `"ok"` | 离开成功 |

---

#### ROOM_INFO（24）— 服务端主动推送

以下事件触发推送（向房间内所有成员广播）：

- 有人加入或离开房间
- 游戏结束（result 变为非空）

Body 为 JSON 字符串，见 [ROOM_INFO JSON](#room_info-json)。

---

#### START_GAME（25）

仅**房主**（座位 0）可发送，需房间内恰好 2 人且游戏未进行中。

| 方向 | body 格式 | 说明 |
|------|-----------|------|
| C→S | 空 | — |
| S→C 成功 | `"ok"` | 开始成功（同时广播 GAME_STATE）|
| S→C 失败 | `"err:login"` / `"err:no_room"` / `"err:not_owner"` / `"err:need_2"` / `"err:playing"` | 见[错误码](#错误码) |

---

#### GAME_ACTION（27）

在游戏进行中，**轮到自己操作时**发送。

| 方向 | body 格式 | 说明 |
|------|-----------|------|
| C→S | `"attack"` 或 `"defend"` | 攻击 / 防御 |
| S→C 成功 | `"ok"` | 操作成功（同时广播 GAME_STATE）|
| S→C 失败 | `"err:login"` / `"err:no_room"` / `"err:not_playing"` / `"err:not_turn"` | 见[错误码](#错误码) |

---

#### GAME_STATE（26）— 服务端主动推送

以下事件触发推送（向房间内所有成员广播）：

- START_GAME 成功后（初始状态）
- 每次 GAME_ACTION 处理完毕后（状态变更）
- 有成员断线且游戏进行中（`result="opponent_left"`）

Body 为 JSON 字符串，见 [GAME_STATE JSON](#game_state-json)。

---

### JSON 结构说明

#### ROOM_INFO JSON

> 用于 LIST_ROOMS 响应的数组元素，以及 ROOM_INFO 推送的 body。

```json
{
  "roomId":  "r1234567890123",   // 房间唯一 ID（字符串）
  "name":    "快来玩",            // 房间名
  "players": 2,                  // 当前人数（0–2）
  "state":   0,                  // 0=等待中/结束, 1=游戏中
  "result":  ""                  // 见下方 result 枚举
}
```

#### GAME_STATE JSON

```json
{
  "hp":     [85, 60],   // 双方 HP，[座位0的HP, 座位1的HP]，初始值 100，最小值 0
  "turn":   0,          // 当前操作方座位号：0 或 1
  "round":  3,          // 已进行的回合数（从 0 开始，每次 GAME_ACTION 成功 +1）
  "result": ""          // 见下方 result 枚举
}
```

#### result 枚举

| 值 | 含义 |
|----|------|
| `""` | 游戏进行中（或未开始）|
| `"p1_win"` | 座位 0 获胜（座位 1 的 HP 归零）|
| `"p2_win"` | 座位 1 获胜（座位 0 的 HP 归零）|
| `"opponent_left"` | 对方主动离开或意外断线 |

---

#### 错误码

| 错误字符串 | 含义 |
|------------|------|
| `err:login` | 未登录 |
| `err:in_room` | 已在某个房间中（需先离开）|
| `err:no_room` | 指定房间不存在 |
| `err:full` | 房间已满（2 人）|
| `err:not_owner` | 非房主，无权执行此操作 |
| `err:need_2` | 房间人数不足 2 人，无法开始 |
| `err:playing` | 游戏已在进行中 |
| `err:not_playing` | 游戏未在进行中 |
| `err:not_turn` | 当前不是你的回合 |

---

## 游戏逻辑说明

### 座位与回合

- **座位 0**：创建房间的玩家（房主），先手
- **座位 1**：加入房间的玩家，后手
- `GAME_STATE.turn` 表示当前**可操作**的座位号
- 每次操作后 `turn` 切换（`0 → 1 → 0 → ...`），`round` 自增

### 操作效果

| 操作 | 效果 |
|------|------|
| `attack` | 对方 HP −10（最低降至 0）|
| `defend` | 本回合无伤害 |

> 两种操作均消耗一个回合，`turn` 和 `round` 都会更新。

### 胜负判定

每次 GAME_ACTION 处理完毕后检查：

- 若**某方 HP ≤ 0**，游戏结束：`state=0`，`result` 设置为 `"p1_win"` 或 `"p2_win"`
- 若**对方断线**（`on_disconnect`），游戏结束：`state=0`，`result="opponent_left"`

胜负结果通过 GAME_STATE 广播给房间内所有人。

---

## 常见问题排查

### 页面无法连接 / "连接错误"

1. 确认 3 个后端进程均已运行，且按顺序启动（游戏服 → 网关 → 桥接）。
2. 确认端口未被占用：`ss -tlnp | grep -E '9000|9001|9080'`
3. 若访问地址为 `http://`（非 https），使用 `ws://`；若为 `https://`，必须使用 `wss://`。

### 登录成功后大厅为空 / 房间列表不更新

- 点击「刷新列表」按钮主动拉取。
- 服务端不主动广播房间列表变化，需手动刷新。

### 点击「开始游戏」按钮置灰 / 无法点击

- 按钮在房间人数 < 2 时禁用，等待另一名玩家加入。
- 非房主看不到此按钮。

### 游戏中对方不响应

- 查看对方浏览器标签是否正常连接。
- 连接断开时服务端会自动触发 `opponent_left` 结算。

### 编译报错：找不到 OpenSSL

```bash
# Ubuntu/Debian
sudo apt install libssl-dev

# CentOS/RHEL
sudo yum install openssl-devel

# macOS (Homebrew)
brew install openssl
export PKG_CONFIG_PATH="$(brew --prefix openssl)/lib/pkgconfig"
```

### 编译报错：找不到 yaml-cpp

```bash
# 快速绕过（H5 游戏不需要）
cmake .. -DCHWELL_USE_YAML=OFF -DCHWELL_BUILD_EXAMPLES=ON
```

---

## 接入自定义客户端

只需实现以下逻辑即可接入：

```
1. 建立 WebSocket 连接（ws:// 或 wss://）
2. 设置 binaryType = 'arraybuffer'
3. 封包：[cmd(2B BE)][len(2B BE)][body]
4. 拆包：循环读取，按 len 切割 body（处理粘包）
5. 实现各命令的收发逻辑（参见上方命令详解）
```

C++ 客户端可复用 `examples/game_commands.h` 中的命令号常量。

wscat 快速验证（需 `npm install -g wscat`）：

```bash
# 连接桥接
wscat -b -c ws://localhost:9080

# 发送 LOGIN（cmd=10, body="test"）的十六进制帧（手动构造）
# 0x000A 0x0004 "test"
```

---

## 已知限制

| 项目 | 说明 |
|------|------|
| 无持久化 | 游戏状态仅在内存中，进程重启后所有房间和会话丢失 |
| 无 TLS | ws_tcp_bridge 为明文传输，生产环境需在前置 Nginx 等处终止 TLS |
| 无身份验证 | LOGIN 仅绑定昵称，无密码或 Token 验证，不可用于生产 |
| 无心跳踢出 | 服务端不主动断开长期不发心跳的连接 |
| 房间上限 2 人 | 当前游戏逻辑硬编码为 1v1 |
| 单进程游戏服 | 不支持水平扩展，多实例需自行实现会话路由 |
| 无重连恢复 | 断线后重新登录无法恢复对局状态 |
