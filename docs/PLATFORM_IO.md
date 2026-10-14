# 平台 IO 边界

`Chwell::io` 提供 `chwell::net::IoService`，只依赖 C++17 标准库与 Threads，可在 Linux 和 Windows 独立构建、安装及消费。默认 `Chwell::core` 传递链接该目标，现有服务无需修改链接方式。

| 入口 | 能力与平台要求 |
| --- | --- |
| `chwell/net/io_service.h` | 工作队列；不包含 socket 或 POSIX 系统头 |
| `chwell/net/socket_io.h` | TcpSocket、TcpAcceptor、ErrorCode 声明；原生操作由 `src/net/posix_io.cpp` 提供 |
| `chwell/net/posix_endpoint.h` | 现有 UDP IPv4 endpoint，保留 `sockaddr_in addr_`，只适用于 POSIX |
| `chwell/net/posix_io.h` | 兼容原来的 POSIX 聚合入口，保留以上类型和原先的系统头 |

TCP、WebSocket、HTTP server、TLS、连接池、节点与 RPC 公共声明使用它们所需的独立头，原生系统调用留在实现文件。socket 声明可以在 Windows 编译，本轮只提供 POSIX socket 实现；`int` 原生描述符不能直接保存 Windows 的 SOCKET。

## 独立构建和安装

```bash
cmake -S . -B build-io -DCHWELL_BUILD_CORE=OFF
cmake --build build-io --config Release --parallel 4
ctest --test-dir build-io -C Release --output-on-failure
cmake --install build-io --config Release --prefix /path/to/chwell-io
```

```cmake
find_package(ChwellCore 0.1 CONFIG REQUIRED COMPONENTS io)
add_executable(worker main.cpp)
target_link_libraries(worker PRIVATE Chwell::io)
```

```cpp
#include "chwell/net/io_service.h"
#include <thread>

chwell::net::IoService io;
std::thread worker([&] { io.run(); });
io.post([&] {
    // 执行应用工作。
    io.stop();
});
worker.join();
```

关闭 core 后不探测 YAML/Protobuf，不下载 GoogleTest，不构建核心示例；安装包不附带依赖 core 的示例、内容生成器和 C# 库。还可通过 `CHWELL_USE_HTTP_CLIENT=ON` 启用独立 HTTP 客户端，此时需要发现 CURL。明确启用 Consul、EntitySchema、数据库或核心 OpenSSL 后端仍要求 core，配置会报告冲突。完整 core 使用 Linux 的 epoll/eventfd，非 Linux 应关闭 core。

`ChwellCore_WITH_CORE` 和 `ChwellCore_core_FOUND` 报告包是否提供核心目标，请求缺失的 REQUIRED component 会在配置阶段失败。`ChwellCore_io_FOUND` 报告工作队列目标；头文件安装不等于对应实现已经构建。

导出目标向 MSVC 消费者传递 `/utf-8`，以正确解析包含中文注释的公开头文件。

## 生命周期与兼容

IoService 保持原来的单次运行语义：多个线程可执行 run，多个生产者可 post；单 runner 按队列顺序取任务，多 runner 不保证完成顺序。回调在队列锁外执行，可再次 post 或 stop。

stop 唤醒所有 runner，已执行的回调自行结束，尚未执行的任务不再派发；停止后的 run 立即返回，队列不能重启。post 的 void 接口不表示任务已执行或可在停止后恢复。回调异常由调用者处理，队列不捕获异常。销毁前必须停止并 join 所有 runner，应用决定排空、补偿或持久化未完成的工作。

TcpSocket 保留单描述符所有权和 move 语义，close、read、write、shutdown 改为调用 POSIX 实现。已关闭 socket 的读写返回 -1 并设置 EBADF；shutdown 可唤醒阻塞读，close 必须在并发操作结束后执行。TcpAcceptor 禁止复制，避免重复关闭监听描述符。聚合头保持源码兼容；静态库布局和内联实现发生变化后，旧二进制需重新编译，0.1 包不承诺 ABI 兼容。

## 验证与后续边界

跨平台测试覆盖多生产者/多 runner 的恰好一次派发、回调再次入队、空闲停止竞态和停止时允许在执行的回调完成。Linux socket 测试验证 move、描述符释放、错误值、shutdown 唤醒和真实 loopback accept；原有 TCP、网关与集群测试继续验证接入。

CI 从根目录构建不含 core 的包，安装后移动路径，使用独立消费者链接 Chwell::io，并检查缺失 core 的组件请求失败。完整安装包也验证独立 IO 目标；Linux ASan/UBSan、TSan 包含队列和 socket 测试。

这是平台隔离的第一阶段。后续还需抽离 epoll 事件分发接口、定义能容纳各平台原生 socket 的句柄及错误模型，并实现目标平台后端；UDP endpoint 的原生地址表示也需要兼容迁移。可靠 UDP 在业务明确需要时另行推进。
