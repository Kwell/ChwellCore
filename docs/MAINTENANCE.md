# ChwellCore 维护记录

## 2026-10-09：配置加载与热更新可靠性

本轮从本地 `488cf52` 及已有未提交改动继续维护，重点修复 `Config`。原有游戏、运维、支付等模块的未提交工作保留在工作区。

### 已完成

- 统一 conf、JSON、环境 profile 和 reload 的加载路径，先在临时对象中解析全部文件，再在同一配置锁内发布。
- 任一文件不可读、JSON 无效或读取期间时间戳变化时拒绝更新，保留当前配置、组件、文件列表和成功加载的时间基线。
- 环境层按 `default.conf → default.json → env.conf → env.json` 覆盖，重载保留各文件原始格式和顺序；JSON 叠加加载继续监控基础文件。
- 热加载失败后可继续重试；多个 Config 实例独立维护时间基线；成功后的回调在锁外执行。
- 删除端口或线程数配置、回滚无这些键的快照时恢复默认值；`set()` 后继续应用环境变量覆盖。
- JSON 校验完整对象、分隔符、数字和转义，解码 Unicode 及代理对，限制对象深度为 64；不支持数组，null 保持跳过语义。
- 添加独立测试工程 `tests/config` 和 Windows CI 作业 `config-windows`，同步中英文文档。

### 验证

本机使用 Windows、MSVC 19.43、C++17、CMake，复用已有固定版本 GoogleTest 源码：

```powershell
cmake -S tests/config -B build-config `
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=F:/XiaomiMiMoProjects/ChwellCore/build/_deps/googletest-src
cmake --build build-config --config Debug --parallel 4
ctest --test-dir build-config -C Debug --output-on-failure
```

结果：44 个 GoogleTest 用例全部通过。新增加载可靠性测试最初复现 9 个失败，JSON 校验和转义测试随后复现 19 个失败，修复后均通过。构建启用 `/W4`，最终配置测试构建无警告。测试日志及 XML 报告在忽略的 `build-config` 目录内。

完整框架依赖 Linux / POSIX，本机未配置 WSL。PR #57 首轮远端验证中，Windows 配置测试、Linux 全量构建测试和 TSan 通过；ASan 中 486 个单元测试有 485 个通过，仅已有的 `TaskQueueTest.StopAndWait` 时序断言失败，没有报告内存错误，集成测试也通过。

该测试提交任务后立即停止队列，却要求至少一个任务已执行，受工作线程调度影响。测试现改用 promise 确认任务已开始，再验证 `stop()` 返回时任务完成、运行数为零、完成数为一；任务捕获共享状态，避免异常路径留下悬空引用。本机 MSVC 下连续重复 100 次通过，随后重新运行完整远端 CI。

随后 ASan 单元测试进程停滞超过 30 分钟，取消后日志仅包含 CTest 的测试启动信息，无法定位具体用例。CI 改用详细输出并为每个测试程序设置 600 秒超时，使后续停滞能自动失败并保留已执行用例的日志；全部测试仍正常运行。

### 后续维护入口

- 在 Linux 上验证当前所有未提交模块的全量构建、单元/集成测试和 ASan/TSan。
- 检查根 CMake 中动态加载库链接是否应独立于 YAML 选项，目前 `${CMAKE_DL_LIBS}` 位于 YAML 分支。
- 分模块复核已有的写回缓存、跨服路由、GM、支付等新增实现，再整理对应提交。

行为兼容说明：显式传入的文件列表现在要求所有文件可读，缺失文件不再被静默跳过；可选路径请先筛选或使用 `load_for_env`。Windows 时间戳检测仍为秒级，Linux/macOS 为纳秒级。
