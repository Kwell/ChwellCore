# 运行时插件版本替换

`RuntimePluginStore` 支持同进程替换无状态的规则计算插件。玩家状态、业务调度和资源所有权留在宿主；插件接收字节请求并返回字节结果。持有同一个 `RuntimePluginSnapshot` 可以让一次逻辑操作始终使用同一版规则。

## C ABI 与规则实现

插件包含 `chwell/service/runtime_plugin_abi.h`，导出 `chwell_runtime_plugin_v1()`，返回 `ChwellRuntimePluginV1`。宿主严格检查 ABI 版本、结构大小、插件名、非零版本号、invoke 指针与保留字段；不匹配时保留当前版本。

插件名为 1..128 字节的 ASCII 字母、数字、`_`、`-`、`.`；版本号是非零 uint64 发布序号，与网络协议、内容包和存储 schema 的版本分开管理。ABI 中只传固定宽度整数、当前平台 size_t 和缓冲区指针，不传 C++ 类、容器或跨模块释放的内存。Windows 使用 cdecl；插件需匹配宿主平台、架构、结构布局和调用约定。完整 C 语言规则见 [独立示例](../examples/runtime_plugin/rule.c)。

`invoke` 返回 0 表示成功，并设置 written 不超过宿主容量。请求、响应默认上限各为 1 MiB，调用者可降低响应容量；失败、输出长度非法或异常时不替换宿主输出。插件不得抛异常、保留输入/输出指针、启动异步任务、注册回调/定时器或执行外部业务副作用。同一镜像的调用串行；不同发布镜像可并行执行。

## 发布、读取与回滚

```cpp
#include "chwell/service/runtime_plugin.h"

chwell::service::RuntimePluginStore plugins("example.damage", "/srv/chwell/plugins");
std::string error;
if (!plugins.publish("/srv/chwell/plugins/damage-v1.so", 0, &error)) {
    // 记录 error，保留当前版本。
}
auto rules = plugins.acquire();
if (rules) {
    std::vector<std::uint8_t> output;
    if (rules->invoke({0, 0, 0, 10}, output, &error, 4)) {
        // rules->version() 是本次计算使用的发布版本。
    }
}
// 成功发布将 generation 从 0 提升到 1。候选版本必须高于进程发布高水位。
plugins.publish("/srv/chwell/plugins/damage-v2.so", 1, &error);
// old rules 仍调用 v1；之后 acquire() 返回 v2。
plugins.rollback(2, &error); // 恢复前一版，generation=3；高水位不降低。
plugins.clear(3, &error);    // 停止新调用获取，generation=4；旧快照仍然有效。
```

每次加载先把源库复制为同目录下唯一的 `.chwell-plugin-*` 镜像，再加载、校验描述信息，最后在锁内发布不可变快照。这样即使多次发布同一个源路径，也不会复用仍驻留的旧镜像。副本放在源目录以保留 Linux `$ORIGIN` 或 Windows 同目录依赖路径。库文件最大 128 MiB，复制前后检查源大小和修改时间；业务应发布不可变版本文件或通过同文件系统原子重命名更新源路径，不能原地覆盖正在被复制的库。

`publish`、`rollback`、`clear` 都比较 expected_generation；并发管理操作只有匹配当前 generation 的操作能成功。generation 每次成功操作递增；候选失败不改变当前快照、generation 或回滚点。回滚只保留一个前驱，成功后消耗；不能回收已发布版本号。`clear` 清空当前和回滚点，也保留进程高水位。

旧快照、旧版执行中的调用和回滚点共同持有镜像。最后一个引用释放后才关闭库并删除副本；即使 store 已销毁，仍持有的快照也可调用。为避免插件异常造成进程崩溃，应用持有的快照必须覆盖整个调用，禁止缓存裸函数指针；应用不能在同一个 shared_ptr 变量上并发 reset 与 invoke。卡住的插件调用没有强制取消，必须由业务限制计算时间。

允许目录按真实 canonical 路径的目录边界检查，拒绝兄弟目录、`..` 穿越和指向目录外的 symlink。目录和插件属于受信任的部署输入；动态库在读取 descriptor 前已执行加载初始化，此机制不是恶意原生代码沙箱，也不校验签名。目录需可写以保存副本；副本已被 Git/Docker 忽略，正常卸载自动清理，进程异常退出留下的副本由部署工具在确认无进程使用后清理。

插件依赖库、进程内全局状态和静态初始化副作用不在版本切换事务内。依赖库不能跟随本接口自动升级；规则插件应保持无状态且无加载/卸载业务副作用。高水位和回滚历史是进程内状态，应用负责重启后的发布版本选择及集群一致发布。逻辑状态升级使用 [存储迁移](STORAGE_MIGRATIONS.md)，内容表切换使用 [内容快照](CONTENT_VERSIONS.md)。

## 传统 Service 组件插件

旧 `HotReloadManager` 仍使用匹配工具链的 C++ `IPlugin`，导出 create/destroy。绑定 Service 时，只允许在 Init/start 之前 load、Service 停机清理后 unload；运行中或已初始化的组件图不能卸载。`reload` 返回 false，`check_updates` 返回 0；组件图没有事务化替换或状态迁移能力，在线规则替换应使用上面的独立运行时接口。headless 模式只能重建旧 ABI 对象用于验证，不保证同路径读取新的镜像。

安装期间组件、接口和 scoped 回调会标记插件所有者；Install 失败/异常时清理部分注册。卸载先取消插件和组件所有回调，再执行 PreShut/Shut、Uninstall，最后移除接口、组件与插件对象，之后才允许关闭镜像。Service 持有镜像：加载器先销毁时，正在运行的 Service 仍可安全使用组件，并在 stop 时清理；Service 先销毁后，加载器也不会解引用已销毁的 Service。

Uninstall 返回 false/抛异常意味着无法确认插件自有外部资源已排空，宿主会将该镜像保留到进程结束，避免立即卸载其代码。插件必须遵守 scoped 注册协议并主动排空自己的工作线程；宿主无法清理未声明的回调或外部裸指针。

传统 Service 和加载器在所属控制线程操作，不得从插件回调或 Install/Uninstall 钩子重入加载器、操纵 Service 生命周期。卸载前业务必须释放组件/接口裸指针和其他外部引用；跨版本长期引用应该采用 RuntimePluginSnapshot。允许目录的旧 `set_allowed_prefix` 现在也按 canonical 目录边界检查。

## 独立构建与验证

安装 ChwellCore 后：

```bash
cmake -S /path/to/installed/share/ChwellCore/examples/runtime_plugin -B build-rules \
  -DCMAKE_PREFIX_PATH=/path/to/installed
cmake --build build-rules --parallel 4
ctest --test-dir build-rules --output-on-failure
```

C 规则库只使用安装头文件，不链接框架；C++ 宿主链接 `Chwell::core`。示例实际验证 v1 计算 20、v2 计算 30、旧读者仍为 20 和回滚恢复 20。最小、完整和 bundled YAML 三种安装包 CI 都独立构建运行该示例。

`tests/runtime_plugin` 可在 Windows 单独构建，验证真实 DLL 加载、多版本同路径发布、坏 ABI/身份/版本拒绝、目录隔离、并发发布冲突、回滚和旧快照生命周期。执行中的旧调用通过确定性测试门跨越 publish、clear 与 store 析构，确认返回仍来自旧版。Linux 全量测试及 ASan/UBSan、TSan 同时验证新接口和传统 Service 插件的部分安装、停机与析构顺序。
