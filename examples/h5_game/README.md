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

1. **登录**：输入昵称，连接 WebSocket 后发送 LOGIN，网关做会话鉴权。
2. **大厅**：LIST_ROOMS 拉取房间列表，CREATE_ROOM 创建房间，JOIN_ROOM 加入他人房间。
3. **房间**：ROOM_INFO 推送房间状态；房主可 START_GAME（需 2 人在房间）。
4. **对战**：GAME_STATE 推送双方血量与回合；轮到自己时发送 GAME_ACTION（attack/defend），先击倒对方者胜。

## 运行方式

### 1. 编译（需 OpenSSL，用于 WebSocket 握手）

```bash
cd build
cmake ..
cmake --build .
```

### 2. 启动后端（建议开 3 个终端）

- **游戏逻辑服**（端口 9000）  
  `./example_game_server`  
  （当前目录需能读到 `server.conf`，无则用默认 9000）

- **游戏网关**（端口 9001，转发到 9000）  
  `./example_game_gateway_server`  
  （需能读到 `gateway.conf`，无则用默认 9001 / 127.0.0.1:9000）

- **WebSocket 桥接**（端口 9080，供浏览器连接）  
  `./ws_tcp_bridge` 或 `./ws_tcp_bridge 9080`  
  桥接将 WS 流量转发到网关（默认 `127.0.0.1:9001`）。  
  若网关在另一台机器，可设置 `GATEWAY_HOST=网关IP`、`GATEWAY_PORT=9001` 再启动桥接。

### 3. 启动前端

在项目根目录或 `examples/h5_game` 下用任意 HTTP 服务暴露页面，例如：

```bash
cd examples/h5_game
python3 -m http.server 8080
```

浏览器打开：`http://localhost:8080`  
页面内「服务端 WebSocket 地址」默认 `ws://localhost:9080`（本地桥接）；分离部署时改为你的后端桥接地址（如 `wss://api.example.com`）。

### 4. 体验对局

- 打开两个浏览器标签（或一个无痕 + 一个普通），分别输入不同昵称并登录。
- 一个创建房间，另一个在列表中点击「加入」。
- 房主点击「开始游戏」，双方进入对战；轮到自己时点「攻击」或「防御」，先打空对方血量者获胜。

## 协议简述（二进制）

- 格式：`cmd(2B 大端) + len(2B 大端) + body(len 字节)`。
- 命令：HEARTBEAT(3)、LOGIN(10)、LOGOUT(11)、LIST_ROOMS(20)、CREATE_ROOM(21)、JOIN_ROOM(22)、LEAVE_ROOM(23)、ROOM_INFO(24)、START_GAME(25)、GAME_STATE(26)、GAME_ACTION(27)。
- ROOM_INFO / GAME_STATE 的 body 为 JSON 字符串，由服务端推送。
的 body 为 JSON 字符串，由服务端推送。
