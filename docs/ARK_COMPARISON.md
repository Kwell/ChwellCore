# ARK 源码学习与 ChwellCore 对比

调研日期：2026-10-09。本文中的“咱们框架”指本仓库 ChwellCore。

## 1. 范围与结论

- ARK：OpenArkStudio/ARK，master，`5334fd48b3c3d00f8cf37b6ac9042456ad3fd748`。
- ChwellCore：main，`cc3e693c7053e552a368540f9611a27529cf6092`。
- 已获取 ARK 完整主仓库快照，重点阅读插件管理、启动程序、kernel 对象与元数据、配置生成、bus、网络会话、服务发现、部分服务器角色以及构建测试脚本，并对照咱们相应模块。未逐行审计所有插件；外部 Git 子模块未展开。
- 本次没有编译运行 ARK，也没有同机性能测试。本文判断源码结构、已实现行为和验证机制，不判断吞吐、延迟或线上容量。
- ARK 的 master 最后提交日期为 2022-08-23。核对 develop 后发现其相对 master 落后 11 个提交、没有领先提交；README 中“最新功能看 develop”的提示不作为本次版本选择依据。仓库页面更新时间不能等同代码维护时间。

**核心判断：ARK 最值得学习的是“把能力组织成完整开发流程”。咱们已有不少独立能力，主要欠缺的是统一组装、共享数据模型、内容制作工具和可验证的跨进程闭环。ARK 自身也有明显未完成部分，不能把目录、接口和 README 宣传当成现成生产能力。**

## 2. ARK 的架构与关键调用链

### 2.1 统一应用宿主与插件注册

`AFMain` 解析应用名、bus ID、应用配置与日志参数，读取应用配置中的动态库目录、插件配置目录和插件列表，再启动 `AFPluginManager`。插件负责注册接口对应的模块，模块通过 `FindModule<T>()` 查找依赖。

生命周期为 `Init -> PostInit -> CheckConfig -> PreUpdate -> Update -> PreShut -> Shut`。ARK 为需要每帧运行的模块维护独立 `module_updates_`，避免所有模块都进入 Update；模块注册时检查重复名字，并提供移除注册的方法。

咱们已具备同样的七阶段生命周期、组件与插件优先级、动态插件 create/destroy 导出函数，以及配置驱动示例。真正值得补的是配置到组件工厂/插件加载器的统一接线，而不是重新设计生命周期。

源码：[启动配置读取](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/app/AFMain.cpp#L301)、[模块注册和选择性 Update](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/base/AFPluginManager.hpp#L74)、[咱们的 Service](../include/chwell/service/service.h)、[配置示例](../examples/config_driven_demo.cpp)。

### 2.2 对象、属性、表格、容器和场景

ARK kernel 把 Entity、类型化属性 Node、Table、Container 和 ClassMeta 组织在一起。对象携带 GUID、类名、配置 ID、地图与实例信息，支持类事件、字段变更、表格操作、容器操作和进出场景回调。

字段支持名字与生成的数字索引访问。这里的数字索引主要来自按字段顺序生成的枚举，**不能视为跨版本稳定的协议字段 ID**；插入或重排字段可能改变编号。

这是一套元数据驱动的对象系统。源码不足以支持“高性能 archetype ECS”的判断，咱们也没有必要为借鉴它而把现有 Component 全面改成动态反射。

源码：[实体创建](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/kernel/src/AFCKernelModule.cpp#L133)、[元数据加载](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/kernel/src/AFCClassMetaModule.cpp#L149)、[地图与实例](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/kernel/src/AFCMapModule.cpp#L66)。

### 2.3 同一份元数据驱动同步与持久化序列化

字段元数据声明 `sync_view`、`sync_self`、`save`、`real_time`：分别控制向观察者同步、向自身同步、存储序列化，以及立即同步还是延迟归并。

实际调用链已核对：字段变更进入 `AFClassCallBackManager::OnNodeCallBack`；实时字段立即触发同步回调，其他字段加入延迟集合；kernel Update 触发 `OnDelaySync`，生成同步数据。Game 模块按可见性标记生成 Protobuf 消息；`EntityToDBData` 按 save 标记筛选属性与表格。

这是 ARK 最有价值的设计：业务数据定义、同步权限、变更通知和存储序列化共用一套描述。但生成 DB Protobuf **不等于** 已有数据库服务、可靠写入、迁移和灾难恢复。当前 `src/server/db` 未提供完整服务实现。

咱们的 StateSyncRoom、ORM Entity、PersistableEntity、WriteBackCache 和 SchemaCodegen 是分开的。已有脏字段记录，但 WriteBackCache 的写回调用 `repo_->save`，不能据此宣称已实现数据库字段级增量更新。

源码：[实时与延迟分流](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/kernel/src/AFClassCallBackManager.cpp#L120)、[同步回调接线](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/kernel/src/AFCKernelModule.cpp#L568)、[存储序列化筛选](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/kernel/src/AFCKernelModule.cpp#L1336)、[游戏可见性同步](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/server/game/src/AFCGameNetModule.cpp#L119)、[咱们的状态同步](../include/chwell/sync/state_sync.h)、[写回缓存](../include/chwell/storage/orm/writeback_cache.h)。

### 2.4 内容制作与配置工具链

ARK 提供 Excel 输入、XML 元数据与配置资源输出、C++/C# 名字与索引定义生成，以及插件/模块模板生成工具。运行时 ClassMetaModule 加载类型定义，ConfigModule 按元数据构造静态配置对象。

值得借鉴的是“策划修改表格 -> 构建时生成 -> 服务启动校验”的工作流。现有生成器不是完整类型安全 SDK，也没有证明具备全面引用校验、兼容性迁移或原子热发布。生成脚本中还存在需要修正的条件表达式和依赖旧接口的代码，不适合直接复制使用。

源码：[生成实体与配置定义](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/build/tools/config_tool/config_tool.py#L176)、[配置资源生成](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/build/tools/config_tool/config_tool.py#L339)、[运行时配置加载](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/kernel/src/AFCConfigModule.cpp#L81)、[咱们的生成器](../include/chwell/codegen/schema_codegen.h)。

### 2.5 网络、bus 和多服务组织

ARK 的 bus 配置统一描述应用类型、连接关系、进程 ID、外网与内网端点；消息头携带来源/目的 bus 和 actor 等信息，MsgModule 提供按应用类型或 bus ID 发送的方法。

网络基于外部 zephyr/Asio，I/O 回调解析数据并放入会话队列，再由 Update 派发消息和事件。TCP、WebSocket 和 RDP 源码存在；RDP 服务和客户端显式使用 `zephyr::use_kcp`，可以确认它接入了 KCP，而非仅有“可靠 UDP”的接口命名。咱们已有 TCP、UDP、WebSocket、TLS 和逻辑线程，尚未找到 KCP 接入及通用 HTTP 客户端。

ARK 将 master/router/world/game/login/proxy 等角色做成独立插件与配置；角色划分清晰，但部分请求处理与转发函数为空或被注释，因此只能认可其组织方式，不能把整个角色目录认定为完整游戏后端。

源码：[bus 配置](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/bus/src/AFCBusModule.cpp#L54)、[消息发送](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/bus/src/AFCMsgModule.cpp#L126)、[TCP 会话边界](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/net/src/AFCTCPServer.cpp#L36)、[KCP 接入](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/net/src/AFCRDPServer.cpp#L121)、[HTTP 客户端](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/net/src/AFCHttpClientModule.cpp#L32)。

## 3. 建议借鉴的地方

下列表格记录调研时的状态；随后实现与合并情况见第 9–14 节。

下面的依赖图、ABI 校验、回滚、稳定字段 ID 等是根据咱们代码提出的增强方案，不是宣称 ARK 已经完整实现这些能力。

| 优先级 | 借鉴方向 | 咱们当前状态 | 适合咱们的落地方式 | 验收标准 |
| --- | --- | --- | --- | --- |
| P0 | 统一应用宿主与角色配置 | 有 Service、PluginManager、HotReloadManager、配置解析与示例，缺少统一组装流程 | 增加 AppHost，配置选择内置组件工厂与可选动态插件；先覆盖 gateway/game 两个角色 | 同一可执行程序以不同配置启动；缺失插件、重复注册、非法参数导致明确启动失败 |
| P0 | 接口注册与模块管理 | get_component 线性 dynamic_cast 查询，组件归属和移除机制有限 | 增加类型/接口注册表、重复检查、注册所有者和注销令牌；依赖声明与拓扑排序 | 缺依赖与循环依赖在启动前失败；卸载后无残留处理器、定时器和组件引用 |
| P0 | 服务身份与连接拓扑 | NodeRegistry、SessionLocator、MemoryServiceDiscovery 主要维护本进程状态 | 将发现后端做成可替换插件；接入成熟 etcd/Consul 客户端，明确租约、重连、事件顺序和下线语义 | 两个独立进程能发现彼此；节点退出后收敛；注册中心重启后恢复；不会把本地 mock 当远端成功 |
| P1 | 共享 EntitySchema | ORM、状态同步与代码生成分别定义数据 | 构建时生成类型化结构和元信息，含显式字段 ID、默认值、约束、自身/可见范围/存储权限 | 新增一个字段不用同时手工维护存储与同步映射；私有字段不外泄；字段重排不改变协议编号 |
| P1 | 实时/延迟变更同步 | StateSyncRoom 已有差异与快照回调，缺少上述共享策略 | 可配置立即发送或按 tick 合并，同一实体同一字段只发最终值；对接 AOI | 高频重复修改能被归并；进视野收到快照；退出视野停止同步；记录归并前后消息量 |
| P1 | 策划配置流水线 | 有服务配置和基础 Entity 源码生成，没有完整内容表流程 | 提供 Excel/CSV 导入到统一 schema，构建时输出数据和 C++/客户端定义；接 CI 校验 | 重复 ID、类型错误、失效引用和客户端禁用字段给出文件/行/列错误；错误产物不替换旧版本 |
| P1 | 可运行的多服务参考工程 | 有大量单模块示例和 Dockerfile，缺少一条统一验收链 | 提供 gateway + 两个 game + 真实发现/存储的最小示例，含客户端、配置和容器编排 | 从连接、路由、状态同步到持久化实际跑通；演示节点失联与重连；CI 执行跨进程测试 |
| P2 | 平台适配和传输扩展 | 完整网络核心依赖 Linux/POSIX；Windows 只验证独立 Config | 先隔离平台 I/O 后端；有业务需求再接入成熟 Asio/KCP/HTTP 客户端 | 在支持的平台运行真实网络测试；KCP 做丢包、乱序和 MTU 测试；HTTP 做超时取消测试 |
| P2 | 插件构建与发布 | 核心主要为一个 chwell_core 目标 | 保留轻量核心，按需拆出传输、存储与工具目标；发布 manifest、版本与依赖清单 | 外部工程能通过导出 CMake targets 消费；最小构建无需业务无关依赖 |

优先级含义：P0 是下一轮基础建设，P1 是接着完成的数据与开发流程，P2 按实际场景投入，不代表所有项都应立即实现。

## 4. 相比 ARK，咱们确实欠缺的地方

| 对比项 | ARK 已有的具体内容 | 咱们的欠缺与影响 |
| --- | --- | --- |
| 配置到运行时的组装 | 统一 app 参数、插件清单、插件配置目录和动态库加载 | 当前更依赖业务示例手工 add_component；部署不同角色缺少统一宿主 |
| 对象模型的一致性 | Entity/Node/Table/Container/ClassMeta 和事件共同工作 | ORM、同步、AOI、地图等未共享同一数据定义，字段变化容易多处修改 |
| 可见性和存储策略 | 字段 mask 实际参与同步和 DB 数据序列化 | 目前需要业务自行约定公开/私有/存储字段，更容易漏过滤或映射不一致 |
| 内容工具 | Excel 资源、元数据、C++/C# 定义与插件模板 | SchemaCodegen 是基础实体骨架，缺少面向策划的完整导入校验和生成流程 |
| 插件粒度 | 每个插件单独 SHARED 目标及 interface/include/src 结构 | 虽有插件 API，核心构建仍偏整体；模块依赖、发布和替换边界需要完善 |
| 跨平台设计 | WIN32/APPLE/Linux 分支、Asio 网络及 Linux/macOS 构建工作流 | 完整框架尚不能据 Windows Config 测试宣称 Windows 支持；网络后端依赖 POSIX/epoll |
| 可靠 UDP 与外部 HTTP 接入 | RDP 显式使用 KCP，HTTP 客户端实现异步请求 | 咱们 UDP 不等于可靠传输；服务治理、支付等外部接入缺少通用 HTTP 客户端 |
| 地图实例与实体事件的联动 | kernel 和 MapModule 统一对象创建、切图、进出场景 | 咱们已有 SLG map、AOI、房间等模块，但未形成相同的统一场景实体流程 |

此外，咱们有几项自己的生产化短板，不能因为 ARK 也不完整就忽略：

- **Redis 失败语义必须优先修正。** DNS、socket、连接和认证失败可能切换到内存 mock 并返回成功。跨进程锁、缓存和在线状态因此可能变成本地数据，调用方却不知道。mock 应显式开启，生产默认应返回失败并可观测。依据：[RedisClient::connect](../src/redis/redis_client.cpp)。
- **“全局会话视图”目前不是分布式视图。** SessionLocator 是本进程 mutex + unordered_map，需要外部存储、一致性约定及迁移期间的冲突处理。依据：[SessionLocator](../include/chwell/cluster/session_locator.h)。
- **热重载需要真实版本替换验证。** reload 在旧库仍驻留时再次加载同一路径，加载器可能复用旧镜像；新插件安装和旧插件卸载的注册项也可能互相影响。需要不同版本产物路径、注册所有权、在途调用排空与可回滚切换。现有代码不足以证明安全无损热更新。依据：[HotReloadManager](../src/service/hot_reload.cpp)。
- **启动失败检查还不等于事务式启动。** InstallAll 失败后未回滚已经成功安装的插件；Service 的 Init 失败路径也没有完整的逐组件回滚记录。依据：[PluginManager](../src/service/plugin.cpp)、[Service](../include/chwell/service/service.h)。

## 5. 咱们比 ARK 更好的地方及边界

| 优势 | 源码/验证依据 | 结论边界 |
| --- | --- | --- |
| 回归测试和动态检查更充分 | 最近 PR #57 验证包含 486 个单元测试、9 个集成测试；CI 包含 Linux 测试、Windows Config、ASan/UBSan、TSan。ARK test 主要注册 8 个基础工具测试，Linux/macOS CI 脚本显式关闭测试 | 说明验证机制和覆盖的行为面更丰富，不说明所有代码可靠；mock 测试不能替代真实外部依赖和集群故障测试 |
| 生命周期错误更容易阻止启动 | 咱们 Service 检查 Init/PostInit/CheckConfig/PreUpdate 的 bool；ARK 管理器调用这些模块方法后忽略其返回值并返回 true | 这是具体正确性优势；咱们仍欠完整回滚和异常安全 |
| 最小使用方式更轻 | 可以直接作为 C++ 库组合 Service/Component；Protobuf、YAML、MySQL、MongoDB、OpenSSL 可按构建选项选择。ARK 顶层要求 Protobuf，默认网络依赖 Boost 或独立 Asio 路径 | 指接入方式与依赖选择，不能推出更高运行性能；咱们外部 CMake 包发布也需要完善 |
| 服务治理工具较丰富 | 已有 PrometheusRegistry、限流、熔断、TrafficSplitter 和对应测试；ARK 此快照未发现同等内建模块 | 表示已有可用基础工具，仍需与请求入口和运维后端统一接线；TraceContext 是日志关联基础，不等于完整 OpenTelemetry 链路 |
| 帧同步场景有更直接的入口 | FrameSyncRoom 有输入收集、帧号、快照与超时兜底，StateSyncRoom 提供差异/快照回调，均有测试；ARK 已读 kernel 主要围绕实体属性和场景同步 | 在开发房间制帧同步玩法时咱们入口更直接，不代表完整客户端预测、回滚、确定性模拟或更优延迟 |
| 存储调用有统一抽象 | StorageInterface、Repository、AsyncStorageAdapter 支持组合，异步适配器可把同步 I/O 移到线程池并限制队列 | 是结构上的便利，不是原生异步数据库或可靠持久化保证；真实 MySQL/MongoDB 故障场景仍需验证 |

源码：[咱们 CI](../.github/workflows/ci.yml)、[ARK 测试定义](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/test/CMakeLists.txt)、[ARK CI 关闭测试](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/.github/workflows/build-ubuntu.sh)、[ARK 忽略初始化返回值](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/base/AFPluginManager.hpp#L273)、[咱们帧同步](../include/chwell/sync/frame_sync.h)、[Prometheus 输出](../src/metrics/prometheus_metrics.cpp)、[异步存储适配](../include/chwell/storage/async_storage_adapter.h)。

TCC/Saga、支付适配器、热重载等不能仅凭存在类名计为“全面领先”：TCC/Saga 是当前进程中的协调基础，缺少持久事务日志与自动故障恢复证据；支付部分有骨架实现；热重载上述风险尚待解决。

## 6. ARK 中不宜照搬的部分

1. **忽略生命周期返回值。** 上层 Start 虽检查阶段返回值，但阶段内部没有传播模块失败；应落实逐项失败检查、错误上下文和回滚。
2. **用无序容器遍历当依赖顺序。** 生命周期遍历 module_instances_；不能作为确定的依赖顺序，咱们应显式建依赖图。
3. **按表格顺序自动编号当长期协议。** 生成 enum 方便，但字段重排会影响编号。咱们应显式 ID、保留删除编号并校验兼容性。
4. **未完成的注册中心接线。** NetServiceManager::HealthCheck 基本为 TODO/注释；etcd Keepalive 空实现；Consul GetHealthServices 请求被注释，返回的 Future 没有正常完成路径。不能据插件名称认定已有自动服务发现。
5. **接口/部署例子与实际构建脱节。** 默认插件 CMake 注释掉 consul/mysql/redis/http 等，game.app.conf 却列出 AFConsulPlugin；启用前应检查构建产物与 manifest 一致性。
6. **服务器角色骨架当完整转发系统。** Proxy 的 OnOtherMessage 等是占位；部分角色启动失败调用 exit(0)，会让外部工具看到成功退出状态。
7. **容器模板当可直接上线。** Kubernetes 示例使用本地镜像策略与项目专用配置，且直接 exec 的 args 含 `${HOSTNAME##*-}`，没有 shell 就不会执行这段参数展开；没有看到配套探针。必须重新验证启动参数、镜像、配置与健康检查。
8. **过多全局状态和宏隐藏依赖。** 注册、断言、单例和回调链增加定位难度。可以保留插件边界，采用显式依赖与所有权，不必复制整套机制。

源码：[健康检查](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/net/src/AFCNetServiceManagerModule.cpp#L45)、[etcd 续租](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/etcd_client/src/AFCEtcdClient.cpp#L136)、[Consul 查询](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/consul/src/AFCConsulModule.cpp#L101)、[默认插件构建](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/plugin/CMakeLists.txt)、[代理占位实现](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/src/server/proxy/src/AFCProxyNetModule.cpp#L129)、[Kubernetes 参数](https://github.com/OpenArkStudio/ARK/blob/5334fd48b3c3d00f8cf37b6ac9042456ad3fd748/kubernetes/photon_game_deployment.yaml)。

尚未验证：ARK 当前环境完整构建是否成功、外部 zephyr 的具体版本行为、插件组合的兼容性、实际服务发现恢复、真实数据库端到端写入、生产负载、Lua 集成与安全热重载。Lua 资源文件的存在不能证明有可用脚本插件。

## 7. 推荐推进顺序

1. **运行基础：** 修正 Redis 默认失败语义；补全插件/组件注册所有权和启动回滚；实现 AppHost 与可检查的 manifest。先把现有组件装配稳。
2. **跨进程闭环：** 接真实发现后端，明确会话定位与路由边界；完成两个 game 节点的注册、路由、失联和重连测试。
3. **数据与内容流程：** 引入可选 EntitySchema，联通 ORM、可见性同步与配置；生成代码优先，保持现有简洁组件写法。
4. **开源可用性：** 发布可运行参考工程、导出构建目标、贡献说明与版本化样例；再按业务需求补 KCP、跨平台网络和更多外部插件。

当前已经按此顺序推进，阶段进展见下文。单纯增加插件和业务类数量不会解决启动、跨进程状态和数据一致性问题。

## 8. 开源复用边界

ARK 是 Apache-2.0，咱们是 MIT。借鉴架构并独立实现可以保持咱们的授权方式；直接复制或修改 ARK 源码时，需要保留相应版权和 Apache-2.0 许可说明、标明修改，并按许可处理上游 NOTICE（本次主仓库快照未发现独立 NOTICE，外部依赖需另查）。复制部分不能直接抹去原许可、统一标成 MIT。

比较调研只新增报告。后续实现按第 7 节推进，独立编写，没有复制 ARK 源码。

## 9. 第一阶段实现进展

已实现运行基础的首批改动：Redis 默认使用真实连接，连接失败不会自动切换到内存；插件组件按所有者记录并逆序卸载；启动失败按实际初始化记录回滚，包含部分失败和异常；新增 AppHost，通过现有配置和组件工厂装配服务，并严格校验 manifest。

旧网络模式停机现在会等待 I/O 工作线程退出，再清理和移除组件。新增回归覆盖连接拒绝、认证/选库失败、断连、生命周期回滚、插件归属、非法配置与工厂失败。使用与迁移契约见 [APP_HOST.md](APP_HOST.md)。

验证状态：PR #58 已合并，Windows、Linux、ASan/UBSan 与 TSan CI 通过。后续依赖图增量见第 13 节，接口注册表与注销令牌见第 14 节；安全动态插件替换仍待推进。

## 10. 第二阶段实现进展

PR #59 已合并，实现可选 Consul/libcurl 后端、TTL 健康检查、DiscoveryRouter 与数字 IPv4 TCP RPC 传输。独立 game 进程验证注册、路由、TTL 失联、会话失效、端点/incarnation 变化后的重连、注册中心重启恢复与优雅注销。五项 CI 通过，包括真实 Consul 多进程场景。契约与限制见 [DISCOVERY.md](DISCOVERY.md)。

## 11. 第三阶段首批实现

可选 EntitySchema 以显式稳定 ID 统一类型、默认值、约束、持久化与可见性，生成 C++ 实体接入 Repository。SchemaSyncRoom 支持 owner/public 快照、tick 合并、立即发送与 AOI 订阅接口；server 字段不进入客户端元数据或同步。标准库 Python 工具校验 JSON schema、版本演进与 CSV 单表内容，成功后原子生成头文件。

PR #60 已合并，Linux、Windows、ASan/UBSan、TSan 和真实 Consul CI 已通过。用法与边界见 [ENTITY_SCHEMA.md](ENTITY_SCHEMA.md)。后续增量补充字段声明的内容引用和 JSON 内容目录：对全部 CSV 校验目标 ID，包括默认值、自引用和循环引用；错误保留原头文件，引用策略进入版本演进检查。Excel、C# 生成、客户端线协议和内容热切换仍待推进。

## 12. 第四阶段首批实现

新增独立参考工程 v1，通过安装后的 `find_package(ChwellCore)` 消费 namespaced 构建目标，联通 AppHost、角色/装备内容、ORM 往返、权限同步、TCP 回显与停止。CMake 包版本 0.1.0，安装生成工具、文档、参考工程和许可证，使用相对安装路径并在消费者机器重新发现外部依赖；未承诺跨版本 ABI。新增 [贡献说明](../CONTRIBUTING.md) 和 [包契约](PACKAGING.md)。

Linux CI 新增最小包、全可选依赖包与 FetchContent YAML 包的安装/移动/独立构建验证，并覆盖真实 TCP 关闭、引用错误保留产物和目录增删表的依赖更新。完整框架仍需 Linux/POSIX；没有因此新增 Windows 网络支持或生产部署能力。

PR #61 已合并，八项 CI 已通过，包括三种安装包场景。

## 13. 组件依赖顺序增量

补齐运行基础中的显式依赖图：组件可按运行时名称声明 prerequisites，AppHost 的 depends_on 按 manifest 入口名解析并映射工厂返回的运行时名称。初始化前统一校验缺失、空目标、自依赖与循环依赖；依赖优先，ready 节点用 priority/原稳定顺序决定先后。循环报告实际路径，不把被阻塞的后继误报为环。规划器独立于平台 I/O，迭代处理深层依赖。

Service 在插件 Install 完成后规划全部组件；正常关闭的 PreShut/Flush/Shut 逆序，失败初始化沿实际记录回滚。AppHost 错误配置不替换旧 host，Service/AppHost 暴露启动错误。安装参考工程增加 ReferenceContent -> ReferenceGame 启动与反向关闭验证。PR #62 已合并，八项 CI 通过。此处管理组件生命周期，不改变插件安装/卸载顺序；完整契约见 [APP_HOST.md](APP_HOST.md)。

## 14. 接口与注册所有权增量

新增显式接口注册表，支持多继承指针调整、重复/不兼容/外部所有者检查和插件卸载前移除绑定。消费者仍声明生命周期依赖；接口查询不自动注入依赖，借用指针不能跨所有者移除保留。

事件、协议处理器与定时器新增 move-only 注销令牌。令牌可交给 Service，按组件或插件归属统一清理；关闭、启动回滚及部分注册失败时先取消回调，再清理组件。取消等待其他线程的在途回调，已复制快照不再进入业务函数，自取消的重复定时器不再复活。旧注册 API 保留，只有显式使用新令牌并托管的注册项自动清理。

安装参考工程实际使用 ContentReader 接口和归属事件订阅，FrameSync 超时定时器采用注销令牌。便携测试与 Linux 生命周期/路由测试接入 CI；契约及线程限制见 [REGISTRATIONS.md](REGISTRATIONS.md)。此增量仍不保证动态库安全热替换。

## 15. 多服务与真实持久化参考增量

新增独立 [cluster_reference](../examples/cluster_reference/README.md)：TCP 客户端经网关登录，
发现两个 Game 节点并固定会话路由，生成玩家实体接入真实 MySQL Repository 和权限同步。
持久化先于成功响应，数据库错误不当作记录不存在；节点/网关/注册中心恢复后重新登录。
附标准库 Python 客户端、Docker Compose 和独立安装包构建，CI 验证真实外部依赖故障与落库。
实现只参考角色划分，未复制 ARK 源码。

此处的 JSON 是示范载荷，SessionLocator 仍是网关本地索引，分布式会话增量见第 16 节。
稳定客户端同步协议见第 17 节。后续依次推进：Excel/C# 内容工具与热切换/迁移、安全插件版本替换，再补生产部署
和按需的跨平台网络/KCP/外部 HTTP。

## 16. 分布式会话所有权与恢复增量

新增 MysqlSessionStore，以真实 MySQL 保存玩家归属、持久递增 epoch 和数据库时间租约。
所有权校验与文档 read-modify-write 在同一 InnoDB 事务中完成；旧 epoch、已过期租约、
错误 Game 进程身份不能继续写入。SessionLocator 只缓存本地路由，Compose 扩展为双网关。
每次启动自动生成 Game incarnation，避免同 generation 参数重启时旧请求被接收。

真实数据库双连接测试覆盖抢占、并发增量、回滚、过期及重连；双网关进程测试覆盖同时登录、
暂停/杀死网关后的接管、旧 RPC/续租/释放和 Game 重启。数据库断连 fail closed，不自动重放。
契约、已有键表迁移及主库持久性前提见 [SESSION_OWNERSHIP.md](SESSION_OWNERSHIP.md)。
此增量尚未实现跨玩家事务、幂等请求日志、无损主库容灾或生产吞吐保证。

## 17. 稳定客户端同步协议增量

独立实现 `CHWS` wire v1，在 EntitySchema/SchemaSyncRoom 输出之上定义 schema/version、
stream、entity、完整 64 位序号和类型标签。C++ 与标准库 Python 客户端共享 golden bytes，
原子应用完整快照/增量；丢包、旧包、错误 stream 和越权字段不改变既有缓存。
参考网关为每个 viewer/entity/stream 分配序号，登录协商协议及 schema 版本，重连清空旧基线。
编码端与 replica 校验 owner/public 字段；server 字段不进入同步包。契约见
[SYNC_PROTOCOL.md](SYNC_PROTOCOL.md)。传输认证、加密、framing、ACK 和自动重传不由本协议提供。
