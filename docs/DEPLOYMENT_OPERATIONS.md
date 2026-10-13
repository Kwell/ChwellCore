# 参考集群的探针、排空与部署验证

此契约适用于 `examples/cluster_reference`，完整服务需要 Linux。参考工程已验证真实依赖故障、排空、替换和逻辑备份恢复；账号系统、业务传输 TLS、跨地域数据库容灾与生产容量仍需应用建设。Compose 使用本地演示凭据，Consul 为 dev 模式。

## 独立操作端口

设置 `CHWELL_OPS_PORT` 启用 GET-only HTTP 操作接口；默认不启用。`CHWELL_OPS_BIND` 默认 `127.0.0.1`，只接受数字 IPv4。绑定失败使进程启动失败。Compose 在私有容器网络使用 `0.0.0.0:9300`，不向宿主发布操作端口；应通过网络策略只允许探针与监控访问。接口不提供管理写操作或认证。

| 路径 | 成功条件 | 失败行为 |
| --- | --- | --- |
| `/live` | 主控制循环和业务 worker 最近 15 秒有进展；worker 排空结束后仍存活 | 503，控制/worker 卡住或服务已停止 |
| `/ready` | AppHost 启动成功、未排空、存活、最近 3 秒内的依赖检查成功，且至少一个可用 Game | 503，保留进程以等待依赖恢复 |
| `/metrics` | 独立线程读取缓存状态和原子计数 | Prometheus text 格式；不执行数据库/RPC |

Game 的依赖检查包含真实 MySQL `SELECT 1` 和 Consul 注册/心跳；数据库失败时撤销注册，恢复后重新注册。Gateway 检查发现查询、至少一个健康节点和自身数据库连接。探针不证明每个远端 Game 可立即处理业务、不保证数据库持久性，也不是一次分布式事务。依赖状态为周期缓存；长请求使状态过期时就绪会失败。仅依赖失联通常不影响存活，不应据此触发无限重启。

HTTP 服务独立于业务队列，最多保留 16 个连接，每个请求头最多 4096 字节，连接绝对期限 2 秒；只支持固定路径和短连接。业务排队、数据库阻塞不会阻塞探针。内置探针只连接回环地址，返回 0 成功、1 不可用、2 参数错误：

```sh
cluster_reference probe 9300 live
cluster_reference probe 9300 ready
```

主进程启动输出的 `READY role` 表示启动完成；是否有可用路由应以 `/ready` 为准。Compose 的角色 healthcheck 使用 ready；Docker Compose 不会单凭 unhealthy 自动重启容器。部署平台应分别配置 startup、liveness、readiness 探针；建议 startup 允许 60 秒，liveness 每 5 秒一次并连续失败 3 次再处理，readiness 每 2 秒一次。

## 排空和替换

`SIGUSR1` 开始不可逆排空并保留操作端口；`SIGTERM`/`SIGINT` 开始相同排空，结束后退出。控制循环在约 20ms 内关闭新业务输入、立即撤销 ready；Game worker 撤销 Consul 注册，随后继续处理已接收的输入。排空以宿主已接收的完整帧为边界，网络中尚未收到的字节和不完整帧不属于可完成请求。

`CHWELL_DRAIN_SECONDS` 默认 15，范围 1..60 秒。到期后不再启动下一条业务请求，未处理输入计入 `chwell_drain_dropped_inputs_total`。正在执行的 DB/RPC 不被强制中断，仍按其既有超时与事务规则完成；这不是严格的进程退出时间上限。业务 worker 结束时释放 Gateway 租约，预算不足的剩余租约由数据库 TTL 过期，不逐玩家延长预算。排空期间不再续租，等待过久的请求可能返回 session_lost。

worker 完成后 `drained=true`；SIGTERM 再保留 1 秒给网络发送队列，最后执行 AppHost 停机。此窗口是尽力发送，不提供客户端 ACK、响应送达或自动重放保证。请求超时、断连或 outcome_unknown 后按 [请求幂等契约](REQUEST_IDEMPOTENCY.md) 查询原 ID 的结果；需要显式重复时必须沿用相同 ID 和参数，不能换新 ID。部署终止宽限期应覆盖排空预算、当前 DB/RPC 超时、清理及发送窗口；Compose 使用 30 秒。业务复杂度或请求上限变化后须重新测量。

本地双网关替换示例：

```sh
docker compose -f examples/cluster_reference/compose.yml kill -s SIGUSR1 gateway
docker compose -f examples/cluster_reference/compose.yml exec -T gateway cluster_reference probe 9300 ready
# 上一步应返回 1。通过私有操作网络 GET /live 检查 drained=true，再替换该实例。
docker compose -f examples/cluster_reference/compose.yml restart gateway
docker compose -f examples/cluster_reference/compose.yml exec -T gateway cluster_reference probe 9300 ready
```

真实入口代理先停止向旧 Gateway 分配新连接，再等待排空。现有 TCP 会话需要客户端重新登录，替换进程从 MySQL 恢复已提交数据。Game 替换会使其绑定会话失效；应用不能据此宣称玩家会话无损热迁移。先启动不同 ID 的新 Game 并等待 ready，再逐个撤销旧节点；不要同时替换全部节点。Compose 的 `restart` 是单实例流程示例，不是自动滚动发布器。

## 指标与告警

```sh
docker compose -f examples/cluster_reference/compose.yml --profile monitoring up -d
```

Prometheus UI 在 `http://127.0.0.1:9090`，抓取四个角色的私有操作端口。`prometheus.yml` 和 `alerts.yml` 为可运行配置，CI 用真实 promtool 校验；告警出现在 Prometheus 中，通知目的地由应用接入 Alertmanager。

指标包含 ready/live/draining、数据库/发现状态、节点数、Gateway 会话数、业务完成/错误计数、排队输入、正在执行的请求、拒绝输入和排空丢弃输入。错误计数包含未知结果；计数不会宣称业务未提交。指标不含玩家/token 等高基数标签，随进程重启归零。队列和拒绝计数以输入事件为单位，不等于消息数。

示例告警覆盖探针失联、worker 无进展、非排空状态下持续未就绪和排空丢弃。生产阈值、错误率/延迟分布、数据库与 Consul 监控、指标保留和告警通知需要按实际负载完善。

## 停写备份与恢复

备份须同时包含 `cluster_kv`、`chwell_session_leases` 和 `chwell_request_results`；epoch 不得回退、删除或重置，不能遗漏已提交请求的结果记录。先排空并停止所有写入进程，包括外部迁移任务，再使用匹配构建序列化配置的逻辑备份。备份包含玩家数据，存放在仓库外。如下为本地 Compose 示例，密码通过环境传入，演示备份目录须事先创建：

```sh
export MYSQL_PWD=local-root-password
docker compose -f examples/cluster_reference/compose.yml stop gateway gateway-two game-one game-two
docker compose -f examples/cluster_reference/compose.yml exec -T -e MYSQL_PWD mysql \
  mysqldump --protocol=TCP -h127.0.0.1 -uroot --single-transaction --no-tablespaces --set-gtid-purged=OFF chwell \
  > /srv/chwell-backups/chwell.sql
# 先在隔离数据库演练，验证玩家文档、TTL 和全部 epoch。目标数据库须为空。
docker compose -f examples/cluster_reference/compose.yml exec -T -e MYSQL_PWD mysql \
  mysql --protocol=TCP -h127.0.0.1 -uroot -e 'CREATE DATABASE isolated_restore'
docker compose -f examples/cluster_reference/compose.yml exec -T -e MYSQL_PWD mysql \
  mysql --protocol=TCP -h127.0.0.1 -uroot isolated_restore < /srv/chwell-backups/chwell.sql
```

备份只包含备份时刻的数据，不能用于在线主库无损切换。恢复前必须隔离所有旧写入端，禁止让旧主库和恢复库同时成为权威；旧租约与客户端连接不能恢复使用。按应用要求等待/失效已有租约，保留 epoch 高水位，并核对备份后的丢失窗口。定期在隔离环境检查备份可恢复，单次演练不能代替跨地域容灾。

## 验证入口

`integration_tests/test_deployment_operations.py` 在 ASan/UBSan 下使用真实 Consul/MySQL，覆盖依赖失联与恢复、独立存活、HTTP 慢连接期限、内置 probe、指标、行锁阻塞中的已接收请求排空、后续输入拒绝、租约释放和替换网关读取。Compose 验证角色健康、monitoring 配置、网关替换、卷重启恢复，以及停写后向隔离库还原文档和 epoch。所有验证均消费移动后的安装包或实际 Docker 镜像。
