# Chwell H5 对战小游戏 Demo

包含 **登录鉴权 → 游戏大厅 → 开房间 → 局内对战** 的完整流程。**前端（本目录静态页面）与后端（游戏服/网关/桥接）可分开部署**：前端可放在任意静态托管或 CDN，后端部署在另一台机器或集群，页面通过「服务端 WebSocket 地址」连接后端。

## 前后端分离部署

- **前端（小游戏 Demo）**  
  仅需部署本目录的静态文件（如 `index.html`），可用任意方式托管：
  - 任意 HTTP 静态服务（Nginx、Apache、对象存储静态网站、Vercel/Netlify 等）
  - 与后端不必同机、同域

- **后端（服务端）**  
  部署 ChwellCore 编译出的游戏逻辑服、游戏网关、WebSocket 桥接（见下方「运行方式」），可部署在同一台机器或内网多机；对外只需暴露 **桥接的 WebSocket 地址**（如 `wss://your-api.domain.com`，需在桥接前配置 TLS/反向代理）。

- **连接方式**  
  - 页面内输入框「服务端 WebSocket 地址」填写后端桥接地址（如 `wss://your-api.domain.com`），首次填写后会按域名记住。  
  - 或通过 URL 参数指定，便于固定入口：  
    `https://你的前端域名/游戏路径/?ws=wss://你的后端域名`

## 流程说明

1. **登录**：输入昵称，点击「连接并登录」，WebSocket 建立后自动发送 LOGIN，网关做会话鉴权。
2. **大厅**：LIST_ROOMS 拉取房间列表，CREATE_ROOM 创建房间，JOIN_ROOM 加入他人房间。
3. **房间**：ROOM_INFO 推送房间状态；房主可 START_GAME（需 2 人在房间）。
4. **对战**：GAME_STATE 推送双方血量与回合；轮到自己时发送 GAME_ACTION（attack/defend），先击倒对方者胜。

## 运行方式

### 1. 编译（需 OpenSSL，用于 WebSocket 握手）

```bash
mkdir -p build && cd build
cmake .. -DCHWELL_BUILD_EXAMPLES=ON
cmake --build .
```

### 2. 启动后端（建议开 3 个终端）

> 以下可执行文件均在 `build/` 目录下生成。

- **游戏逻辑服**（端口 9000）  
  ```bash
  ./example_game_server
  ```
  （若当前目录无 `server.conf` 则使用默认端口 9000，线程数 4）

- **游戏网关**（端口 9001，转发到 9000）  
  ```bash
  ./example_game_gateway_server
  ```
  （若当前目录无 `gateway.conf` 则使用默认 9001 → 127.0.0.1:9000）

- **WebSocket 桥接**（端口 9080，供浏览器连接）  
  ```bash
  ./ws_tcp_bridge
  # 或指定监听端口：
  ./ws_tcp_bridge 9080
  ```
  桥接将 WS 流量透明转发到网关（默认 `127.0.0.1:9001`）。  
  若网关在另一台机器，可通过环境变量覆盖：
  ```bash
  GATEWAY_HOST=网关IP GATEWAY_PORT=9001 ./ws_tcp_bridge
  ```

### 3. 启动前端

在 `examples/h5_game` 目录下用任意 HTTP 服务暴露页面：

```bash
cd examples/h5_game
python3 -m http.server 8080
```

浏览器打开：`http://localhost:8080`  
页面内「服务端 WebSocket 地址」默认为 `ws://localhost:9080`（本地桥接）；分离部署时改为你的后端桥接地址（如 `wss://api.example.com`）。

### 4. 体验对局

1. 打开两个浏览器标签（或一个无痕 + 一个普通）。
2. 分别输入不同昵称，点击「连接并登录」——连接建立后自动完成登录。
3. 一个玩家点击「创建房间」，另一个在列表中点击「加入」。
4. 房主点击「开始游戏」（需 2 人在房间），双方进入对战。
5. 轮到自己时点「攻击」（-10 HP）或「防御」（不扣血），先打空对方血量者获胜。

## 协议简述（二进制）

- **帧格式**：`cmd(2B 大端) + len(2B 大端) + body(len 字节)`
- **命令号一览**：

  | 命令 | 值 | 方向 | body 说明 |
  |------|----|------|-----------|
  | HEARTBEAT   | 3  | C↔S | 客户端发 8 字节小端 int64 时间戳（ms）；服务端原样回显 |
  | LOGIN        | 10 | C→S | body = 昵称字符串（UTF-8）|
  | LOGIN        | 10 | S→C | body = `"login ok: <id>"` 或 `"login failed: ..."` |
  | LOGOUT       | 11 | C↔S | body 任意或为空 |
  | LIST_ROOMS   | 20 | C→S | body 为空 |
  | LIST_ROOMS   | 20 | S→C | body = JSON 数组，每项：`{id, name, players, state}` |
  | CREATE_ROOM  | 21 | C→S | body = 房间名（UTF-8，可为空） |
  | CREATE_ROOM  | 21 | S→C | body = 房间 ID，或 `"err:..."` |
  | JOIN_ROOM    | 22 | C→S | body = 房间 ID |
  | JOIN_ROOM    | 22 | S→C | body = `"ok"` 或 `"err:..."` |
  | LEAVE_ROOM   | 23 | C↔S | body = `"ok"` 或为空 |
  | ROOM_INFO    | 24 | S→C | body = JSON：`{roomId, name, players, state, result}` |
  | START_GAME   | 25 | C→S | body 为空 |
  | START_GAME   | 25 | S→C | body = `"ok"` 或 `"err:..."` |
  | GAME_STATE   | 26 | S→C | body = JSON：`{hp:[p1hp,p2hp], turn, round, result}` |
  | GAME_ACTION  | 27 | C→S | body = `"attack"` 或 `"defend"` |
  | GAME_ACTION  | 27 | S→C | body = `"ok"` 或 `"err:..."` |

- ROOM_INFO / GAME_STATE 的 body 为 JSON 字符串，由服务端推送。

## 已知限制与注意事项

- 当前游戏服为单进程演示，**不做持久化**，重启后数据清空。
- WebSocket 桥接为纯透明转发，无 TLS；生产环境请在前置 Nginx/HAProxy 终止 TLS。
- 心跳超时检测需业务层自行实现（当前服务端不踢出不发心跳的连接）。
- `example_game_server` 为单线程游戏逻辑，并发请求将排队处理。
