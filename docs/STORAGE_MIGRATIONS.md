# 存储 schema 迁移

迁移的是 KV 表中实体 Document 的字段与版本，不是 MySQL 物理列。启用 `CHWELL_USE_ENTITY_SCHEMA=ON`；执行还需要 `CHWELL_USE_MYSQL=ON`。安装包消费者链接 `Chwell::schema`。

## 版本与部署

生成实体的 `EntitySchema::version()` 与生成类 `schema_version` 一致。`to_document()` 写入保留字段 `__chwell_schema_version`，`load_document()` 只接受相同版本；历史无版本文档只允许版本 1 加载。手写 metadata 默认版本 0 保持无版本存储，但拒绝带版本的文档；需要迁移时显式传入非零版本。

即使只添加字段或改变默认值，也需提升版本并注册迁移步骤。生成器 `--previous` 继续拒绝不兼容结构改变；显式迁移不会自动解除构建兼容检查。重命名、类型转换、删除字段的版本应独立生成，并经过迁移评审；删除的字段 ID 仍应保留在新 schema 的 reserved_ids。

先部署具备版本检查的读写端，再执行迁移。更早的二进制不认识版本元数据，仍可能把新文档当旧版覆盖。每个玩家迁移期间先清空写回缓存、暂停该玩家业务与定时持久化，使用当前权威租约执行；迁移后重新加载新实体，不能继续保存迁移前的内存对象。离线迁移需先停写并获取该玩家租约，不能强占仍有效的在线租约。全局批量任务由应用管理清单、进度、限速与租约续期。

## 预演与执行

`StorageMigration(old_schema, new_schema, transform)` 注册一个显式的递增版本步骤，实体名、逻辑表与主键字段 ID 必须相同。`prepare()` 是纯预演，不访问数据库。transform 接收原始旧文档和已补齐旧默认值的目标文档；必须显式转换/删除旧字段，新增字段可使用新版默认值。不要调用容错的数值 getter 进行类型转换，使用严格解析并验证范围。

```cpp
#include "chwell/schema/storage_migration.h"
#include "chwell/cluster/mysql_session_store.h"

// old_schema/new_schema 是明确带版本的 EntitySchema shared_ptr。
chwell::schema::StorageMigration step(old_schema, new_schema,
    [](const auto&, auto& destination, std::string*) {
        destination.set_string("points", destination.get_string("score"));
        destination.erase("score");
        return true;
    });

chwell::storage::StorageDocument original;
auto read = sessions.read_document(lease, "players:" + player_id, original);
if (!read.ok()) return; // 处理 Lost/Missing/Unavailable/Unknown。
chwell::schema::MigrationChange receipt;
std::string error;
if (!step.prepare(original, receipt, &error)) return;
// 预演到这里结束；执行前由应用持久保存 receipt 和任务状态。
auto applied = sessions.compare_exchange(lease, receipt.before, receipt.after.value);
// 条件回滚：业务仍暂停，且已确认执行成功后再运行。
// auto reverted = sessions.compare_exchange(lease, receipt.after, receipt.before.value);
```

预演严格检查旧版本、旧值和约束、存储 key 与 id；拒绝未知字段，禁止改变身份、手动写版本。成功输出规范化的新文档，补齐新默认值并与新版本一起保存；失败不修改输出 receipt，转换异常传播。

输入必须是当前构建的 `Document::to_string()` 规范序列化：解析再序列化必须逐字节相等，防止旧解析器吞掉重复 key、垃圾行或歧义转义。外部手写 YAML、旧序列化库输出和切换 YAML 开关可能不满足此条件，应停写备份后单独核对并规范化，不能直接忽略校验。回滚 receipt 保留原始字节，包括原本缺失的默认字段与版本戳。receipt 含业务数据，应用负责保密、持久化和保留周期。

## 事务、失败与恢复

`read_document()` / `compare_exchange()` 使用与 [会话所有权](SESSION_OWNERSHIP.md) 相同的 MySQL 连接与事务：先锁定并验证玩家租约，再锁定文档；等锁结束后重新读取数据库时间判断 TTL。执行比较完整旧字节和 TTL，仅更新 value，保留 expire_at；提交前重新验证租约到期。版本与字段属于同一 value，所以不会出现只改版本或只改数据。`read_document()` 仅在提交成功后修改输出参数。

所有写端必须遵守同一玩家租约，且应用必须保证租约玩家对应目标 key；框架不推断业务 key 的归属。不能在 `apply()` 回调内嵌套调用这些方法，也不能通过普通 Repository/put 绕过协调。

| 状态 | 含义与处理 |
| --- | --- |
| Ok | 本次事务已确认提交；可持久记录进度并加载新实体 |
| Conflict | 当前字节或 TTL 不同；没有覆盖，重新读取并人工/业务判断 |
| Missing | 文档不存在或已经到期；不会插入或复活 |
| Lost | 租约不匹配或到期；没有提交，停止旧所有者操作 |
| Invalid | 参数或存储 TTL 不合法；修正输入 |
| Unavailable | 提交前失败并断开连接，停止当前操作并排查依赖 |
| Unknown | COMMIT 回应丢失，可能已经成功；禁止自动重跑转换或直接回滚 |

Unknown 后重连、验证/重新取得权威租约，并用 `read_document()` 读取当前状态：与 receipt.after 的字节和 TTL 相等表示当前是目标状态；与 before 相等表示当前是旧状态；其余表示数据发生变化，需要人工或业务恢复。持久保存每条 receipt 与确认进度后，进程重启可按同样方式逐条核对。此接口没有持久操作 ID 或事务审计表，状态相等不证明某个历史操作一定提交过，也不提供全批次原子性。

回滚只能替换当前仍匹配 after 的文档，修改过的字段或 TTL 会冲突。完整字节比较不能识别修改后又还原的 ABA；回滚前必须暂停业务写入并核对迁移后的操作记录，不能把此接口当作任意历史恢复系统。业务已基于新数据产生外部效果时，应用需另行补偿。

## 验证

`tests/test_storage_migration.cpp` 覆盖版本、历史数据、默认值、类型/约束、未知字段、身份、转换失败与预演原子性。`tests/mysql_session/mysql_migration_contract` 在 CI 的安装包消费者中对真实 MySQL 运行双连接条件竞争、版本与 TTL 一起提交、重复执行冲突、原字节回滚、业务修改/TTL 修改拒绝、过期与旧租约保护。`integration_tests/test_mysql_migration_unknown.py` 通过隔离数据库的协议代理丢弃真实 COMMIT 成功响应，验证 Unknown、断线重连核对与禁止盲目重放；另一模式延迟 UPDATE 成功响应直到租约到期，验证真实写入后的事务回滚。
