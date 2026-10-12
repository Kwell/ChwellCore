# 运行时内容版本与热切换

`ContentStore` 是可选 `Chwell::schema` 的服务端内容加载器。生成器把整个 CSV/Excel 目录写成一个独立二进制包，运行时重新校验所有表后一次发布不可变快照。更新内容数据不需要重新编译服务；改变 schema 则需要新的程序版本或另行迁移。

## 生成与加载

```bash
python tools/entity_schema.py --catalog examples/reference_service/content/catalog.json \
  --bundle-output build/content-v1.chct --content-version 1
```

生成之前检查所有 schema、单元格约束和引用，包括未选中表、默认值、自引用与循环引用。包按表名、内容 ID、字段 ID 排序，输入顺序不影响输出。版本必须是非零 uint64，与各表的 schema version 独立。验证失败保留原文件；成功时以临时文件加原子替换写出。支持与 `--output`/`--csharp-output` 同时使用，但多个输出文件分别替换。

```cmake
find_package(ChwellCore 0.1 CONFIG REQUIRED COMPONENTS schema)
chwell_generate_content_bundle(content_bundle content/catalog.json generated/content.chct VERSION 1)
add_dependencies(my_service content_bundle)
target_link_libraries(my_service PRIVATE Chwell::schema)
```

CMake 跟踪全部 schema 和工作簿/CSV；目录增删表时重新配置依赖。生成物应放在忽略的构建目录中，`.chct` 不提交到源码仓库。CSV 包生成只需 Python 标准库，Excel 依赖见 [内容工具](ENTITY_SCHEMA.md)。

生成的实体增加 `content_definition()` 和 `from_content()`。前者提供编译时 schema、完整兼容性契约和引用；后者把已验证的内容行转换为原有类型化实体，并清除 dirty。转换要求行来自相同 schema 对象，否则抛出 `invalid_argument`。

```cpp
#include "player.h"
#include "item.h"
#include "chwell/schema/content_store.h"

using Player = chwell::generated::ReferencePlayer;
using Item = chwell::generated::ReferenceItem;
chwell::schema::ContentStore content({Player::content_definition(), Item::content_definition()});
std::string error;
if (!content.publish_file("content-v1.chct", 0, &error)) throw std::runtime_error(error);

auto view = content.snapshot(); // 初次加载前为 nullptr
const auto* source = view->find("players", "hero");
if (source) {
    auto player = Player::from_content(*source);
    const auto* item = view->find("items", player.get_weapon());
    // 玩家及装备都来自这个 view；不要在操作中重新读取 snapshot。
}

// 在应用选择的发布时机显式调用，例如认证后的运维入口或 tick 边界。
if (!content.publish_file("content-v2.chct", view->generation(), &error)) {
    // 旧版本及回滚目标均不变，error 说明原因。
}
auto current = content.snapshot();
if (!content.rollback(current->generation(), &error)) { /* 无前驱或代次冲突 */ }
```

## 发布、并发与回滚契约

- `snapshot()` 返回线程安全的 `shared_ptr<const ContentSnapshot>`；一个逻辑操作/tick 始终持有同一个句柄，才能保证跨表版本一致。`find()` 未找到表/行返回 nullptr，`rows()` 未找到表抛出 `out_of_range`。行指针和迭代器只在快照句柄仍持有时有效。
- 解析、类型/约束检查和全部引用校验在发布前完成；提交用互斥锁整体替换指针。错误包不会暴露部分新表，也不会改变当前版本或上一版本。旧读者继续读取原快照。
- 调用方传入预期 generation，初次发布为 0。每次成功发布/回滚加 1；不匹配时失败，因此准备较早的候选不能覆盖已经发布的新内容。generation 达到 uint64 上限后拒绝修改。
- 发布版本必须高于本进程成功发布过的最高版本。回滚恢复上一内容版本，但 generation 继续增加，最高已发布版本不降低；回滚后再次发布仍需使用更大的版本号。
- Store 只保留一个前驱。成功回滚消耗前驱，重复回滚失败；下一次成功发布又建立前驱。已有读者自行持有的快照不受这个保留数限制。

兼容性契约是生成器输出的规范化完整 schema JSON，按字节与编译时定义比较，包含类型、默认值、存储/可见性、约束、保留 ID、引用及 schema version。包必须包含 Store 定义的完整表集合及每行的完整字段集合。即使 schema 变更满足构建期演进规则，也不能直接热切换到旧程序；这一步只更新内容值和记录集合。

`publish_file()` 有大小限制并检查短读/额外字节。部署端应发布不可变版本文件，或先写临时文件再原子替换；不要在读取期间原地修改包。发布 API 可从任意线程调用，业务如需 tick 边界生效，应由自己的逻辑线程决定调用时机。这里没有自动文件监控或远程发布权限控制。

版本高水位和回滚目标只存在于本进程内，重启后需由应用选择初始包。多个进程不会自动同步发布；活跃玩家实体、已持久化记录和客户端副本不会因内容切换而自动迁移。业务需要定义删掉内容 ID 时如何处理已有玩家引用，以及哪些新内容参数应该作用于现有实例。

## 二进制格式 CHCT v1

整数使用网络字节序；int64 是二进制补码，double 是 IEEE 754 binary64。字符串是 uint32 字节数后跟严格 UTF-8，允许 NUL。包包含服务端私有字段与内容，只用于服务端分发，不作为公开客户端资产。

| 顺序 | 字段 |
| --- | --- |
| 包头 | `CHCT` 4 字节、格式版本 uint16=1、保留位 uint16=0 |
| 版本 | 内容版本 uint64、表数 uint32 |
| 每表 | 表名字符串、编译契约字符串、行数 uint32，表名严格升序 |
| 每行 | 字段数 uint32；每字段为字段 ID uint32、类型标签 uint8、值，字段 ID 严格升序 |
| 类型标签 | 1=int64（8 字节），2=double（8 字节），3=bool（1 字节，只允许 0/1），4=字符串 |

最大包 16 MiB、单个字符串 1 MiB、64 表、全包 100000 行、每表 schema 1024 字段。拒绝截断、尾随字节、非有限 double、非法 UTF-8、未知标签、缺失/重复字段、重复/空 ID、schema 不匹配和失效引用。格式本身不提供签名、校验和或加密；包的来源认证与完整性由部署系统负责。

## 可运行验证

安装包附带的 [参考工程](../examples/reference_service/README.md) 提供：

```bash
reference_service --content-check content-v1.chct content-v2.chct invalid.chct
```

同一进程加载旧包、拒绝坏包、发布新包，再回滚；同时持有旧/新读者，检查版本及装备属性。CI 在三个可移动安装包中生成真实包并执行这个入口，覆盖玩家从 sword 切换到 staff、坏引用拒绝和恢复原属性。Linux/Windows schema 测试验证整数极值、Unicode/NUL、全部截断位置及并发读写；Linux sanitizer 检查覆盖加载器。
