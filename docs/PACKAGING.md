# 安装包与独立消费者

ChwellCore 的首个 CMake 包版本为 0.1.0，`find_package(ChwellCore 0.1 CONFIG REQUIRED)` 使用同 minor 版本兼容规则。此编号用于构建包发现，尚未承诺跨版本 ABI、存储迁移或网络协议兼容。

```bash
cmake -S . -B build-package -DCHWELL_BUILD_TESTS=OFF -DCHWELL_BUILD_EXAMPLES=OFF \
  -DCHWELL_USE_ENTITY_SCHEMA=ON -DCHWELL_USE_YAML=OFF -DCHWELL_USE_PROTOBUF=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-package --parallel 4
cmake --install build-package --prefix "$HOME/chwell-0.1"
```

```cmake
find_package(ChwellCore 0.1 CONFIG REQUIRED COMPONENTS core schema)
add_executable(my_service main.cpp)
target_link_libraries(my_service PRIVATE Chwell::schema)
```

消费者通过 `CMAKE_PREFIX_PATH` 指定安装前缀。包自动提供 C++17、头文件目录、Threads 和静态库需要的链接依赖，消费者无需手写 pthread/dl 或框架源码路径。

| 目标 / component | 可用条件 |
| --- | --- |
| `Chwell::core` / `core` | 始终可用 |
| `Chwell::schema` / `schema` | `CHWELL_USE_ENTITY_SCHEMA=ON`，传递链接 core |
| `Chwell::consul` / `consul` | `CHWELL_USE_CONSUL=ON`，libcurl >=7.68、nlohmann-json >=3.11 |
| `Chwell::game_proto` / `game_proto` | 找到 Protobuf 并生成 game.pb.h/cc；头文件可直接 `#include "game.pb.h"` |

请求未编译的 REQUIRED component 会在配置时失败。同名别名也可以在源码 `add_subdirectory` 后使用。源码目标名 chwell_core/chwell_schema 等保留。

`ChwellCore_WITH_YAML/MYSQL/MONGODB/OPENSSL` 布尔变量报告包的编译能力，不表示这些后端已经连接到外部服务。

启用 YAML、OpenSSL、MySQL、MongoDB 时，构建阶段必须能找到对应依赖；不会将启用但缺依赖的数据库/TLS 后端当作有效产物。YAML 仍可通过 FetchContent 获取固定上游版本；此路径将 yaml-cpp 库、头文件和 MIT 许可证一并安装，导出为 `Chwell::yaml_cpp`，消费者无需另找系统 YAML。使用系统 YAML 构建的包仍在消费者机器查找 yaml-cpp。Protobuf 保留原来的可选探测行为，缺失时关闭该目标。

安装包按实际编译选项调用 `find_dependency`。YAML、OpenSSL、CURL、JSON、Protobuf 使用上游 CMake 包/查找模块；MySQL/MongoDB 使用附带的查找模块，优先 pkg-config，也支持标准头文件/库探测。导出文件不硬编码构建机的依赖目录，消费者须安装兼容的第三方开发包；这些依赖不会全部静态打包进去。MySQL/MongoDB 查找验证不等于真实数据库故障验证。

安装布局遵循 GNUInstallDirs（默认 include、lib、share；某些平台使用 lib64）。除了头文件和库，还安装 Python 内容生成器、Excel 可选依赖清单、支持 C++/C# 输出的 CMake 生成函数、MIT 许可证、文档与 [独立参考工程 v1](../examples/reference_service/README.md)。包配置按安装位置解析自身资源，安装前缀可移动；移动后消费者应重新配置并使用新的 CMAKE_PREFIX_PATH。系统依赖仍须可发现。

```cmake
# find_package 自动提供此函数，Python 只在生成内容时需要
chwell_generate_entity_schema(content_header content/entity.schema.json generated/entity.h
    CATALOG content/catalog.json PREVIOUS content/released.schema.json)
add_dependencies(my_service content_header)
target_include_directories(my_service PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
```

完整网络框架仍需要 Linux/POSIX。本机 Windows 可以验证可移植模块，不能据此宣称 Windows 整包运行。Linux CI 分别构建最小包、全部可选依赖包和 FetchContent YAML 包，再安装、移动目录、构建独立消费者并执行 TCP/关闭测试；另验证引用和 Excel 错误不会覆盖产物，以及工作簿/CSV/目录增删表会触发重新构建，删除 C# 输出会重新生成。

运行 `python3 integration_tests/test_installed_package.py build-package` 前，需用同一 Python 安装 `tools/requirements-excel.txt`，并准备 .NET 8 SDK。测试把该解释器传给消费者 CMake，实际编译运行 C++ 内容和 C# 客户端契约；普通 CSV 内容构建不需要 Excel 依赖或 .NET SDK。
