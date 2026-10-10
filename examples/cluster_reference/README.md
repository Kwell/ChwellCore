# Persistent cluster reference v1

独立实现的参考工程：TCP 客户端 → 网关 → Consul 发现的两个 Game → 真实 MySQL。
使用 AppHost、DiscoveryRouter、SessionLocator、MysqlSessionStore、TcpRpcTransport、生成的 EntitySchema、
Repository 和 SchemaSyncRoom；没有复制 ARK 源码。完整框架和这个参考服务需要 Linux/POSIX。

## 本地启动

仓库根目录运行（需要 Docker Compose v2）：

```sh
docker compose -f examples/cluster_reference/compose.yml up -d --build
export CHWELL_DEMO_TOKEN=local-demo-token
python3 examples/cluster_reference/client.py --player alice --advance
docker compose -f examples/cluster_reference/compose.yml logs --tail 100
docker compose -f examples/cluster_reference/compose.yml down
```

服务启动后等待两节点注册再登录；`client.py` 遇到错误退出，不自动重试修改。
两个网关端口只映射到宿主机 127.0.0.1:9100/9101；Consul、Game 与 MySQL 不映射到宿主机。
MySQL 数据保存在 `player-data` 卷，重新启动保留；`down -v` 会删除数据。
Compose 中的密码与 token 仅供本地示例。容器使用非 root 用户运行参考服务。
Game 在启动时将 `CHWELL_ADVERTISE_HOST` 解析为数字 IPv4，注册实际 TCP RPC 端点；
每次启动自动生成新的 Game incarnation，网关据此辨识同 ID 的新进程；generation 参数仅为显示标签。

## 源码或安装包消费

```sh
cmake -S . -B build -DCHWELL_BUILD_TESTS=OFF -DCHWELL_BUILD_EXAMPLES=ON \
  -DCHWELL_USE_YAML=OFF -DCHWELL_USE_PROTOBUF=OFF -DCHWELL_USE_MYSQL=ON \
  -DCHWELL_USE_CONSUL=ON -DCHWELL_USE_ENTITY_SCHEMA=ON
cmake --build build --target cluster_reference --parallel 4
```

依赖 libmysqlclient、libcurl、nlohmann-json ≥3.11、Python ≥3.8。开启 YAML 的构建也支持
该工程，但同一数据库中的 ORM 文档必须使用相同序列化配置。不要在 YAML/非 YAML 构建间
直接复用该卷。安装包含本目录，可复制到仓库外，用独立 `cmake -S ... -B ...
-DCMAKE_PREFIX_PATH=安装前缀` 构建；必须安装开启 MySQL、Consul、schema 的框架包。

手动启动参数：

```sh
cluster_reference game http://127.0.0.1:8500 game-one 9201 v1
cluster_reference game http://127.0.0.1:8500 game-two 9202 v1
cluster_reference gateway http://127.0.0.1:8500 9100
```

所有角色设置 `CHWELL_CLUSTER_TOKEN`；网关设置 `CHWELL_DEMO_TOKEN`。
所有角色设置 `CHWELL_DB_HOST`（默认 127.0.0.1）、`CHWELL_DB_PORT`（3306）、
`CHWELL_DB_NAME`（chwell）、`CHWELL_DB_USER`（chwell）、`CHWELL_DB_PASSWORD`；
数据库须事先创建，该用户须有建表/读写权限。`CONSUL_HTTP_TOKEN` 可选。
所有角色启动必须成功连接真实数据库，Game 还须注册服务，没有内存回退。
从旧版本升级已有卷时，先停止旧服务并按 [会话契约](../../docs/SESSION_OWNERSHIP.md) 迁移键列；
不满足 InnoDB/区分大小写条件时拒绝启动。

## 请求与数据契约

这是参考 JSON 载荷，不承诺稳定生产客户端线协议。沿用框架的
`cmd:uint16 | len:uint16 | body` 网络字节序帧，cmd=1；外部 body 是 UTF-8 JSON。
内部 RPC 在 JSON 前包含框架的 4 字节 request ID。参考请求上限 8192 字节。
每条 TCP 连接按顺序请求/响应；Python 客户端只用标准库。

| action | 请求字段 | 行为 |
| --- | --- | --- |
| status | 无 | 当前健康节点、网关本地会话数 |
| login | player、token | hash 选择 Game；取得全局租约后事务加载/建档；绑定当前连接 |
| get | 无 | 当前玩家的 owner 快照 |
| advance | 无 | level +1、gold +10；先提交 MySQL，再返回同步 delta |
| observe | target | 读取目标玩家；按服务端连接身份过滤 public/owner 字段 |
| logout | 无 | 释放精确身份的租约并删除本地绑定 |

玩家 ID 限 1–64 个 ASCII 字母、数字、`_`、`-`。网关忽略修改请求中的 client player/viewer，
使用当前已登录连接身份和网关取得的租约。跨网关拒绝同玩家同时登录；断连或 logout 后可以再次登录。
共享 demo token 只演示入口校验，持有者可以登录任意 demo player，不能当账号认证。
内部 cluster token 用于拒绝未授权 Game RPC；当前传输没有 TLS，部署限可信本地网络。

字段 1=id、10=level 为 public，20=gold 为 owner，30=secret 为 server。
secret 实际存入 `cluster_kv` 的 `cluster_players:<id>` ORM 文档，不进入客户端 packet；
其他玩家只能看到 public 字段。没有业务内存写回缓存，每次请求在租约保护的事务中从 DB 加载，
提交成功后响应。advance 的读取、递增和写入在同一连接/事务中；响应包含十进制字符串 epoch。

网络回调仅入队；独立 worker 串行处理发现、路由、会话、schema 和阻塞 DB/RPC。
参考 inbox 限 128 个活跃流、1024 个数据事件、1MiB 排队字节，另保留断连清理事件；
流空闲 30 秒关闭，过载关闭连接。MySQL connect/read/write timeout 均为 2 秒，
单次 RPC deadline 为 5 秒；这不是全链路服务延迟保证，排队与数据库多次往返仍可能超时。

失败与恢复规则：

- Consul 查询失败撤销路由/本地会话，恢复后重新登录；Game TTL=4 秒，每秒心跳。
- 节点下线、端点或 incarnation 改变使会话失效，不自动将 stateful 修改转发到其他节点。
- Game/网关重启后重新登录从真实 MySQL 加载已提交数据；不恢复旧 TCP 会话。异常退出的网关
  租约最多等待数据库 TTL=8 秒，另一网关才能接管；正常运行每 2 秒续租。
- 数据库错误不会视为“玩家不存在”；当前请求返回错误并撤销会话，后续调用重新连接/登录。
- 租约过期/旧 epoch/错误 Game incarnation 返回 session_lost；旧持有者续租或释放不能影响新租约。
- `storage_unavailable` 表示读取失败，未尝试该业务写入；`outcome_unknown` 表示不能确定写入/
  传输结果，调用方须重新读取确认，不能盲重放。响应丢失也可能已经提交。

所有权校验与文档写入共享 MySQL InnoDB 事务，单玩家行锁协调跨网关请求。
完整契约及主库持久性前提见 [SESSION_OWNERSHIP.md](../../docs/SESSION_OWNERSHIP.md)。
尚未实现跨玩家事务、幂等请求日志、灾难恢复或生产负载验证。

## 自动验证

CI `persistent-reference` 从移动后的安装包独立构建该工程，ASan/UBSan 下启动真实
Consul、MySQL、两个 Game 和 TCP 网关，测试落库、可见性、鉴权、错误请求、重复登录、断连、
TTL 失联、新 incarnation、网关/注册中心/数据库重启，并通过独立 SQL 查询核对持久记录。
同一 job 还执行双连接事务契约、双网关同时登录/暂停/崩溃接管与旧 RPC fencing，构建运行
上述 Compose，验证双网关全局冲突、容器数值端点与卷数据恢复；full 安装包 job 也独立构建该参考工程。
