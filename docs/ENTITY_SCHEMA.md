# 可选 EntitySchema 与内容流水线

这条路径把字段定义用于 C++ 类型化访问、ORM 映射与同步可见性。以 ARK 的共享属性元信息和内容生成思路为参考，独立实现；没有复制其源码。现有手写 Entity、PersistableEntity、StateSyncRoom 可继续使用。

## 构建和运行

完整框架在 Linux/POSIX 上构建，选项默认关闭。运行时只需 C++17；生成器需要 Python 3.8+，只用标准库。

```bash
cmake -S . -B build -DCHWELL_USE_ENTITY_SCHEMA=ON -DCHWELL_BUILD_EXAMPLES=ON
cmake --build build --target example_entity_schema chwell_schema_tests
ctest --test-dir build -R 'chwell_schema_tests|entity_schema_generator|entity_schema_example' --output-on-failure
./build/example_entity_schema
```

Windows 和 Linux 也能独立构建此模块的测试和示例，无需网络层、YAML 或数据库：

```bash
cmake -S tests/schema -B build-schema
cmake --build build-schema --config Debug
ctest --test-dir build-schema -C Debug --output-on-failure
```

示例从 CSV 生成模板实体，写入 MemoryStorage，通过 Repository 读回，再让旁观者订阅快照。两次 level 修改在 flush 时合并成一个最终值；gold 和 secret 不会发给旁观者。示例会检查这些行为并返回非零状态表示失败。

## 字段模型

完整输入见 `schemas/player.schema.json` 和 `schemas/player_defaults.csv`。Schema 显式给出实体名、存储表、版本、主键字段 ID 和删除后保留的 ID。每个字段必须给出：

| 属性 | 支持值与含义 |
| --- | --- |
| `id` | 非零 uint32，稳定编号；不按数组或表格顺序自动编号 |
| `name` | 非保留 C++ 标识符，也是 Document 存储键 |
| `type` | `int64`、`double`、`bool`、`string` |
| `default` | 类型匹配的默认值；double 必须有限 |
| `stored` | 是否进入 ORM Document，与同步权限独立 |
| `visibility` | `server` 只留服务器；`owner` 给拥有者；`public` 给所有订阅者 |
| `min` / `max` | 可选数字范围；int64 边界使用整数，不转为浮点 |
| `max_bytes` | 可选字符串字节上限；生成器按 UTF-8 计数，运行时按 std::string 字节数 |

主键必须是名为 `id` 的持久化 string 字段，默认值为空，以兼容已有 Repository 的存储键约定。创建后显式设定非空主键；非空主键不能通过 setter 修改。加载必须包含非空主键。加载应在加入同步房间前完成。

生成的 `<实体名>` 位于 `chwell::generated`，提供 `field_<name>` 常量、`get_<name>()`、`set_<name>()`、`entity_schema()`、`schema_version`、`content()` 和 `client_schema()`。setter 返回 bool，可通过 string 指针取得错误。没有可绕过验证的可写字段引用。

```cpp
auto p = std::make_shared<chwell::generated::SchemaPlayer>();
p->set_id("player-42");
std::string error;
if (!p->set_level(101, &error)) { /* 超出 1..100，原值不变 */ }
```

## 持久化契约

生成实体继承 PersistableEntity，可使用 `Repository<T>` 和 WriteBackCache。持久化包含 server/owner 字段，忽略 `stored=false` 字段。只有真正改变的持久化字段会标记 ORM dirty；同步发送不清除此标记。直接使用 Repository 保存后，调用方应仅在保存成功后 clear_dirty。

`load_document()` 严格检查全部已知持久化字段的类型与约束，成功后一次替换状态并清除 dirty。未知字段忽略，缺少普通字段采用默认值，瞬态字段恢复默认值。失败保留原值与 dirty；`from_document()` 在失败时抛出 invalid_argument。已有 Repository 的 find 会传播此异常，应用需要在存储读取边界处理。

数字映射使用完整 int64 和可往返的 double 精度，禁止 NaN/Inf。Document 字符串序列化沿用现有 YAML 或 key=value 后端，没有新增存储格式或自动数据库迁移。

## 同步与 AOI 接线

SchemaSyncRoom 是运行在实体所属逻辑线程的适配器。业务把 AOI enter 接到 subscribe，leave 接到 unsubscribe；只有订阅者收到该实体的字段。owner 与 viewer 必须来自服务器认证后的身份；相等时可见 owner 字段。server 字段始终排除。

```cpp
chwell::sync::SchemaSyncRoom room; // 默认 Tick
room.add_entity(p, authenticated_owner_id);
room.subscribe(p->id(), authenticated_viewer_id, send_packet);
p->set_level(2);
p->set_level(3);
room.flush(); // 每个观察者每个字段至多一个最终值
```

订阅立即发完整可见快照。每个观察者维护自己的已送达基线，tick 结束按基线产生差量；同一字段改回原值不发送，新增订阅者直接得到最新状态。Immediate 模式通过 `room.set_value()` 立即发送；直接调用生成 setter 的变更仍需 flush。统计中 submitted_changes 仅统计 room.set_value 提交的实际修改，packets / delivered_fields 统计回调成功的快照和差量。

发送回调抛异常时保留该观察者基线供下一次 flush 重试，同时继续其他观察者与实体；本轮结束后传播首个异常。首次快照失败不建立订阅，可重新 subscribe。回调成功仅表示应用接受数据，不等于网络远端 ACK；应用负责可靠传输、重连快照和错误处理。

禁止发送回调重入修改房间，也不得通过共享实体指针在回调内修改或加载实体。已挂接实体的 ID 与 owner 固定，迁移需先移除再添加。remove/unsubscribe 不发送销毁消息，业务应处理 AOI leave 的客户端清理。

SchemaPacket 包含实体 ID、snapshot 标记、稳定字段 ID 与类型化 Value。它不是现有 StateValue 的宿主字节序编码；应用需要定义版本握手、序列化与传输。`client_schema()` 只输出 owner/public 字段元数据，不含 server 字段或 CSV 内容。生成头文件包含服务器元数据和内容，仅用于服务端构建，不能作为公开客户端资产。

## 内容生成与演进

```bash
python tools/entity_schema.py schemas/player.schema.json \
  --csv schemas/player_defaults.csv --output build/generated/player_schema.h
# 将发布过的旧 schema 作为兼容性基线
python tools/entity_schema.py schemas/player.schema.json \
  --previous path/to/released.schema.json --output build/generated/player_schema.h
```

CSV 标题对应字段名，必须含 id，未提供的列采用 schema 默认值。数字和布尔严格解析，重复列、未知列、重复/空 ID、越界数据会失败，字段错误含文件、行、列。全部输入验证通过后才原子替换一个头文件；错误不会覆盖已有产物。不要在构建错误后发布残留旧产物。

字段重排输出不变。使用 `--previous` 时，删除 ID 必须进入 reserved_ids，旧保留编号不能回收，字段名不能迁移到另一 ID。字段名称、类型、持久化、可见性和约束改变需要单独设计迁移，本工具拒绝这些变更；新增字段或修改默认值必须提升 schema version。运行时不会自动协商版本。

CMake 可复用 `cmake/ChwellEntitySchema.cmake`：

```cmake
include(path/to/ChwellEntitySchema.cmake)
chwell_generate_entity_schema(my_schema path/to/entity.schema.json generated/entity.h
    CSV path/to/content.csv PREVIOUS path/to/released.schema.json)
add_dependencies(my_server my_schema)
target_include_directories(my_server PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(my_server PRIVATE chwell_schema)
```

当前完成 CSV 单表校验与生成，还没有 Excel 导入、表间外键引用检查、C# 生成、二进制内容格式、内容热切换或在线存储迁移。这些可继续扩展同一流程。
