# 持久化请求幂等与结果恢复

`MysqlSessionStore::apply_once` 在玩家租约保护的同一 MySQL 事务中修改业务文档并写入请求结果。相同请求 ID 和相同业务命令再次到达时，返回已提交结果，不再次调用业务回调。`request_result` 查询已提交结果，不执行业务修改。

此契约限定于同一权威 MySQL 主库内的文档操作；不提供支付、消息发送或其他外部副作用的 exactly-once 保证。数据库丢失已提交数据、回滚备份、双主写入或手动删除日志会破坏保证。

## 请求身份与结果

去重范围为 `(document_table, player, request_id)`，与网关、Game、租约 epoch 和进程生命周期无关。租约替换后，新的有效所有者可恢复原结果；旧所有者不能读取或重放。不同玩家或业务 KV 表可以使用相同 ID。

通用接口接受 1..128 字节请求 ID、1..8192 字节 canonical command 和最多 1 MiB 的结果；ID 和内容均按精确字节比较，支持 NUL。结果是业务数据，不应保存网络包、同步序号、stream、租约或认证信息。

command 必须编码操作名、业务语义版本以及全部影响结果的规范参数。同 ID 不同 command 返回 Conflict；不运行回调，也不修改输出。没有自动 JSON 规范化或哈希推断：应用负责确定性序列化。改变操作语义时必须提升命令版本。请求 ID 在发送前创建并持久保存；一次业务意图始终使用同一 ID，不能因超时创建新 ID。

```cpp
chwell::cluster::DurableRequestResult outcome;
const std::string id = persisted_request_id;
const std::string command = "increment:v1:1";
auto status = authority.apply_once(lease, id, command, outcome,
    [&](chwell::storage::StorageInterface& transaction, std::string& result) {
        // 仅用本事务 connection 读取、校验和修改该玩家的文档。
        auto old = transaction.get(player_key);
        if (!old.ok) return old;
        result = compute_new_value(old.value);
        return transaction.put(player_key, result);
    });
// 只有 status.ok() 时 outcome 才替换；replayed=true 表示返回已存结果。
// Unknown 后重新连接/取得有效租约，再用同一 id/command 查询：
auto recovered = authority.request_result(current_lease, id, command, outcome);
```

执行先锁定并校验玩家租约，再读取结果行。已有记录时直接返回；无记录时调用业务回调，插入结果记录，重新检查数据库租约期限，最后提交。并发执行和结果查询都被同一玩家租约行串行化；查询不会把正在提交的请求误读为已经确认不存在。回调只允许当前事务中的文档 get/put/remove，不能操作事务、调用嵌套 store 方法、执行外部 I/O 或提前产生客户端可见副作用。

回调拒绝、异常、结果过大或执行中租约过期会回滚文档和日志。异常继续传播。只有确认 COMMIT 成功才替换调用者结果；COMMIT 响应丢失返回 Unknown，输出保持原值。Unavailable 表示当前事务未确认成功，不自动重试。即使连接中断发生在提交前，也应使用同一 ID 恢复。

结果查询的 Missing 表示本次租约保护的读取没有已提交记录；不能据此断言旧网络请求以后不会到达。应用可显式提交相同 ID 和相同 command，所有迟到副本仍经过原子去重；不得换新 ID。只读查询本身的 COMMIT 响应丢失也可能返回 Unknown，可以重复查询。

## 参考服务与客户端

参考服务的 `advance` 必须携带 `request_id`，允许可选整数 `steps=1..100`，默认 1。业务规则为 level 加 steps、gold 加 `10*steps`，仍受 schema 上限约束。规范命令是包含 action=`advance`、version=1 和 steps 的确定性 JSON；客户端 player/viewer、stream、同步序号和租约不会进入命令。ID 限 ASCII 字母、数字、`_`、`-`。

首次成功的响应仍返回同步 delta，另带：

```json
{"request":{"id":"operation-123","replayed":false,"result":{"schema_version":1,"level":2,"gold":10}}}
```

同 ID 重复 advance 返回原 `request.result`、`replayed=true`，并在当前租约下重新读取玩家，返回当前同步快照。原业务结果是历史结果；快照可能已包含后续操作，不能把旧结果当当前状态。原 delta、stream 和 sequence 不持久化或重放。重新读取/编码失败时返回 outcome_unknown，仍可查询持久业务结果。

登录后发送 `action=request_result`、原 request_id 和原 steps，可获得 `ok/request_id/result`；不产生同步包，不递增同步序号，不修改业务。无记录返回 request_not_found，参数不匹配返回 request_id_conflict。网关使用已登录连接的玩家和租约，拒绝匿名查询，不接受客户端越权 player 或旧 fence。

Python `Client.ask('advance')` 可生成 UUID 并在发送前保存为 `last_request`，仅保存在该 Python 对象中；应用应把 ID 和参数持久保存到自己的 outbox/UI 状态。客户端不自动重试。CLI 在发送前打印 pending_request，也支持显式 ID 和结果查询：

```sh
python3 examples/cluster_reference/client.py --player alice --advance --request-id operation-123 --steps 1
# 断连或 outcome_unknown 后重新登录并查询，期间始终保留原 ID/参数。
python3 examples/cluster_reference/client.py --player alice --result --request-id operation-123 --steps 1
# 需要显式重发时沿用相同 ID/参数；不要生成新 ID。
```

此机制不恢复旧 TCP 会话，也不使同步协议自动重传。引用示例仍使用 demo token；生产账号认证与业务 TLS 需另行接入。

## 日志、升级与备份

`connect()` 创建 `chwell_request_results`，并验证业务表、租约表和结果表均使用 InnoDB，结果表为完整二进制复合主键及预期类型。已存在但不兼容的日志拒绝启动，不能静默改表或回退本地缓存。日志 format 当前为 1，未知格式/异常记录拒绝使用；不会删除后重新执行。数据库用户需既有建表/读写权限。

没有自动 TTL、删除或按进程重启清空日志。删除记录后旧 ID 可能再次执行，因此应用不能随意清理；永久去重需要永久保留身份/结果或另行设计持久 tombstone 和明确的客户端重试期限。当前只限制单条大小，不限制记录总量；容量规划应包含请求日志增长及备份大小。

升级时停止旧写入程序，确保所有修改路径都使用带 ID 的新接口；旧 Game 可能忽略 ID，不能与新 Game 混用并宣称已保证去重。修改 command 语义版本、实体 schema 和日志格式需分别规划，不自动转换历史结果。

停写备份必须同时包含业务文档、`chwell_session_leases` 和 `chwell_request_results`。单独恢复玩家文档或遗漏日志会导致已提交操作再次执行；单独恢复旧日志可能返回与数据不一致的历史结果。数据库备份回退不恢复已丢失的去重记录，恢复期间必须隔离旧写入端。参考 Compose 演练校验这三类记录一同还原。

## 验证

真实 MySQL 双连接契约覆盖并发重复请求、等待中的结果查询、参数冲突、精确二进制 ID/结果、存储表隔离、业务拒绝/异常/超限/过期回滚及所有者替换。协议故障代理分别丢弃 INSERT OK（COMMIT 前）和 COMMIT OK，核对文档与结果日志的原子回滚/提交、输出保留以及同 ID 恢复。非 InnoDB 结果表启动失败。

真实 TCP 测试丢弃服务端已成功生成的 advance 响应，重启原网关和 Game，再从另一网关登录、查询结果并显式重复；独立 SQL 检查没有第二次修改。后续操作后重复旧 ID 返回新快照与原结果，测试同步序号没有被结果查询消耗，并验证匿名/其他玩家不能读取该结果。损坏的历史 JSON 或未知日志格式返回 invalid_request_record，不删除日志或重新执行。上述场景在 CI 的 ASan/UBSan 下运行。
