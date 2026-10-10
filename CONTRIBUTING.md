# 贡献说明

欢迎通过 issue 提供复现步骤，或提交范围清楚的 PR。完整框架使用 C++17 和 Linux/POSIX；配置、发现、字段模型及任务队列提供 Windows 独立验证入口。

修改请解释具体触发条件和修改后的行为，附上相关验证。新模块优先作为可选目标，不强迫已有应用启用外部服务。组件与回调的所有权、启动失败回滚、线程归属和关闭行为应有明确契约。借鉴其他项目的设计时独立实现；引用或引入第三方代码必须保留相应许可与来源。

## 本地验证

```bash
cmake -S . -B build -DCHWELL_BUILD_EXAMPLES=OFF -DCHWELL_USE_ENTITY_SCHEMA=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --timeout 600
```

字段/内容工具改动还应运行：

```bash
python3 tests/test_entity_schema_tool.py
cmake -S tests/schema -B build-schema
cmake --build build-schema --config Debug --parallel 4
ctest --test-dir build-schema -C Debug --output-on-failure
```

涉及导出目标、依赖、安装布局或参考工程时，按 [PACKAGING.md](docs/PACKAGING.md) 构建安装包，再运行 `python3 integration_tests/test_installed_package.py build-package`。该测试会在临时目录安装和移动包，验证独立链接、真实 TCP 回显、停止与生成依赖。

组件依赖改动可在任一平台用 `cmake -S tests/service_order -B build-order` 构建规划器回归；真实 Service/AppHost 的生命周期与插件回滚在 Linux 核心测试验证。无依赖声明时保留稳定 priority 顺序；新增前置依赖需覆盖初始化失败、重试与逆序关闭行为。

## 兼容性与评审

发布过的 schema 保留显式字段 ID；删除后保留编号，使用 `--previous` 检查演进。不要用表格顺序或类成员顺序充当长期协议。改变存储、可见性、约束或引用策略时说明迁移方案。参考工程 schema version=1 作为当前样例版本，CMake 包的 0.1.0 独立管理；两者不能替代客户端协议版本。

测试应覆盖可观测的错误与成功行为，避免只是复述实现。仓库 CI 包含 Linux/Windows 可移植测试、ASan/UBSan、TSan、真实 Consul 多进程测试和三种安装包场景。按修改范围运行本地检查；提交 PR 后检查最终 commit 的全部 CI，不用较早 commit 的结果代替。

PR 描述应让未读讨论的维护者理解问题、行为变化、验证和实际限制。不要把 stub、接口定义或单进程模拟描述成生产验证。
