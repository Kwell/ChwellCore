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
| `reference` | 可选构建期内容引用：`{"table":"items","allow_empty":false}`，仅允许非主键 string 字段引用目标表的 id |

主键必须是名为 `id` 的持久化 string 字段，默认值为空，visibility 为 public，以兼容已有 Repository 的存储键约定与同步包中的实体身份。max_bytes 如设置必须大于零。创建后显式设定非空主键；非空主键不能通过 setter 修改。加载必须包含非空主键。加载应在加入同步房间前完成。

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

字段重排输出不变。使用 `--previous` 时，删除 ID 必须进入 reserved_ids，旧保留编号不能回收，字段名不能迁移到另一 ID。字段名称、类型、持久化、可见性、约束与引用策略改变需要单独设计迁移，本工具拒绝这些变更；新增字段或修改默认值必须提升 schema version。运行时不会自动协商版本。

CMake 可复用 `cmake/ChwellEntitySchema.cmake`：

```cmake
include(path/to/ChwellEntitySchema.cmake)
chwell_generate_entity_schema(my_schema path/to/entity.schema.json generated/entity.h
    CSV path/to/content.csv PREVIOUS path/to/released.schema.json)
add_dependencies(my_server my_schema)
target_include_directories(my_server PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(my_server PRIVATE chwell_schema)
```

## 表间内容引用

字段可声明 `"reference": {"table": "items"}`。allow_empty 默认 false；显式 true 允许空字符串，其他值仍须命中目标表 id。主键不可再声明引用，暂不支持数字外键、任意目标列、复合键或数组引用。

用 JSON 内容目录列出所有相关表，schema/CSV 路径相对于目录文件解析，和运行命令时的工作目录无关：

```json
{"tables": [
  {"schema": "player.schema.json", "csv": "players.csv"},
  {"schema": "item.schema.json", "csv": "items.csv"}
]}
```

```bash
python tools/entity_schema.py examples/reference_service/content/player.schema.json \
  --catalog examples/reference_service/content/catalog.json --output build/generated/player.h
```

`--catalog` 与 `--csv` 互斥，正在生成的 schema 必须在目录中。生成器加载全部表后统一校验，包括没有输出头文件的表，支持自引用和循环引用；重复表名/schema、重复目标 ID、漏配目标表与无效引用都会失败。没有跨表引用时可继续使用原来的单表命令；自引用也可以通过单表 CSV 校验。

遗漏列采用的默认值同样参与校验。错误例如 `players.csv:2:5 (weapon): unresolved reference items.id = 'typo'`；多行引号单元格以记录起始行定位，列号是 CSV 字段序号。缺失列错误定位到该记录的 id 列，并标注 `omitted column, schema default`。整个目录验证通过后才替换当前输出头文件；多个生成目标不是一组文件的事务发布。

```cmake
find_package(ChwellCore 0.1 CONFIG REQUIRED COMPONENTS schema)
chwell_generate_entity_schema(player_content content/player.schema.json generated/player.h
    CATALOG content/catalog.json)
```

CMake 将目录中的每个 schema/CSV 加入 DEPENDS，目录增删表会触发重新配置，更新依赖图。引用元数据不会出现在 `client_schema()`；引用不新增数据库外键，也不校验运行时 setter 或存储加载的关联值。动态修改实体引用时，业务需校验目标并处理内容版本。完整可运行例子见 [参考工程](../examples/reference_service/README.md)。

## Excel 导入与 C# 客户端契约

CSV 和 C# 生成只需 Python 标准库；Excel 是可选依赖，不影响既有构建：

```bash
python -m pip install -r tools/requirements-excel.txt
python tools/entity_schema.py schemas/player.schema.json \
  --xlsx content/players.xlsx --sheet Players \
  --output build/generated/player.h --csharp-output build/client/Player.cs
# 只生成客户端契约也可以；--previous 同样执行版本演进检查
python tools/entity_schema.py schemas/player.schema.json --csharp-output build/client/Player.cs
python tests/test_content_clients.py --require-dotnet
```

仅支持 `.xlsx`，第一行是字段名，必须有文本类型的 id。单工作表可省略 sheet，多工作表必须显式指定。未知/重复列、合并单元格、标题外有数据、重复/空 ID 均报错。完全空白的行跳过；遗漏列采用默认值；已声明列中的空白单元格按空字符串解析，所以数字和布尔列的空白会报错。

接受原生数字、布尔和文本单元格，类型不得隐式转换：数字 id 不能变字符串，数字 0/1 不能变布尔；文本数字及 true/false 沿用 CSV 的严格规则。int64 数字单元格最多允许 15 位，超过该范围必须使用文本，完整支持有符号 64 位范围。公式（包括存在缓存结果的公式）、Excel 错误值和日期单元格一律拒绝。工具不执行公式，也不借助缓存值掩盖输入错误。错误定位示例：`players.xlsx:Players!C2 (gold)`。

内容目录支持混合 CSV 与 Excel，每个条目恰好选择一种内容源，sheet 仅用于 Excel：

```json
{"tables":[
  {"schema":"player.schema.json","xlsx":"players.xlsx","sheet":"Players"},
  {"schema":"item.schema.json","csv":"items.csv"}
]}
```

Excel 与 CSV 共用 schema 约束、跨表引用和演进策略，包含未选中表、遗漏列默认值、自引用与循环引用的校验。`--list-inputs` 列出全部 schema 和工作簿/CSV 路径，CMake 据此更新依赖。

```cmake
chwell_generate_entity_schema(player_content content/player.schema.json generated/player.h
    XLSX content/players.xlsx SHEET Players CSHARP_OUTPUT generated/Player.cs)
```

CMake 也可将 XLSX/SHEET 换为 CATALOG，生成的 C# 文件是受追踪的输出，删除后会重新生成。安装包包含可选依赖清单，安装后的生成函数支持相同参数。依赖安装使用安装前缀下的 `share/ChwellCore/tools/requirements-excel.txt`。

机器有多个 Python 或使用虚拟环境时，配置 CMake 时传入 `-DPython3_EXECUTABLE=/path/to/python`，并使用该解释器安装 Excel 依赖，避免生成器选到另一个环境。

C# 输出位于 `Chwell.Generated.<schema name>` 命名空间，`ClientEntity` 提供 `value_<field name>` 类型化属性及默认值；`Contract` 提供 `field_<field name>` 常量、SchemaVersion、KeyFieldId 和 SchemaJson，字段 ID 与 C++ 完全一致。类型对应 long/double/bool/string，字符串按 UTF-16 转义，支持 Unicode 与 NUL。字段前缀避免 C# 关键字与生成成员冲突。SchemaJson 与 C++ `client_schema()` 使用同一可见性筛选逻辑，排除 server 字段和 reference 元数据；不输出内容行、服务器表名或服务器默认值。

该 C# 类型是数据契约，属性赋值不执行服务器约束，也不包含 CHWS 解码器；客户端仍需按 [同步协议](SYNC_PROTOCOL.md) 处理快照/差量及权限。所有内容验证完成后才写产物，每个文件单独原子替换；磁盘写入失败时多个产物不是一组事务。

后续尚待实现二进制内容格式、内容热切换和在线存储迁移。
