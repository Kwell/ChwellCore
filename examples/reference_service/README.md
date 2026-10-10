# 独立参考工程 v1

此工程通过已安装的 ChwellCore 0.1 包构建，不包含框架源码。content 下保留 version=1 的 schema、稳定字段 ID、CSV 和目录，演示角色装备引用、AppHost 组件启动、MemoryStorage/Repository 往返、owner/public 同步及关闭。

ReferenceGame 声明依赖 ReferenceContent。即使 Game 的 priority 更靠前，内容组件仍先 Init；Game 关闭时内容组件仍可用，随后才清理内容。service.conf 的 depends_on 使用配置入口名，代码声明使用运行时名称；详见 [AppHost](../../docs/APP_HOST.md)。此增量未改变 v1 内容 schema。

完整框架和此工程需要 Linux/POSIX、C++17；生成内容需要 Python 3.8+。最低依赖版本见框架选项和 [构建包说明](../../docs/PACKAGING.md)。本例直接使用服务器给定的 owner/spectator 身份，仅在内存中保存，不接入登录或数据库。

先在框架仓库构建、安装：

```bash
cmake -S . -B build-package -DCHWELL_BUILD_TESTS=OFF -DCHWELL_BUILD_EXAMPLES=OFF \
  -DCHWELL_USE_YAML=OFF -DCHWELL_USE_PROTOBUF=OFF -DCHWELL_USE_ENTITY_SCHEMA=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-package --parallel 4
cmake --install build-package --prefix "$HOME/chwell-0.1"
```

独立工程可来自仓库的此目录，也可复制安装包中的 `share/ChwellCore/examples/reference_service`：

```bash
cmake -S "$HOME/chwell-0.1/share/ChwellCore/examples/reference_service" -B build-reference \
  -DCMAKE_PREFIX_PATH="$HOME/chwell-0.1" -DCMAKE_BUILD_TYPE=Release
cmake --build build-reference --parallel 4
ctest --test-dir build-reference --output-on-failure
./build-reference/reference_service --smoke
```

smoke 使用系统分配端口，只执行一轮更新后退出；验证成功会输出 `PASS: content, ORM, owner/public sync and shutdown`。把 sword 改成不存在的装备 ID 会在生成时失败。修改任一内容输入会自动重新生成，不需要手动清理 build。

构建同时生成 `generated/ReferencePlayer.cs` 客户端契约，只包含 owner/public 字段，不含内容行。内容源也可替换为 Excel，安装可选依赖后在目录条目中使用 xlsx/sheet；规则见 [Excel/C# 工具](../../docs/ENTITY_SCHEMA.md)。

ReferenceContent 显式注册 ContentReader 接口，ReferenceGame 通过接口读取内容。Game 在 Init 中订阅更新事件并交给 Service 管理；smoke 同时检查 PreShut 前订阅已经注销、内容接口在消费者 Shut 期间仍可使用。接口和注册令牌契约见 [REGISTRATIONS.md](../../docs/REGISTRATIONS.md)。

持续运行：

```bash
./build-reference/reference_service --serve build-reference/service.conf
# 另一终端；服务会原样返回 TCP 字节，可用 nc 发送一行查看
printf 'hello\n' | nc -w 1 127.0.0.1 9000
```

SIGINT/SIGTERM 触发正常停止；端口和组件在 service.conf 中配置。监听沿用框架的所有网卡绑定，运行时请使用适合自己的主机网络环境。网络回调仅做字节回显，不修改实体；生成内容、ORM 与同步全部由主更新线程处理。回显不是游戏协议，生产应用需补消息边界、认证和 SchemaPacket 编解码。

已安装包也附带 [包说明](../../docs/PACKAGING.md) 与 [字段说明](../../docs/ENTITY_SCHEMA.md)。schema 变更请保留发布基线并用生成器 `--previous` 检查；改变引用/类型/约束等策略需另行迁移。这里的 v1 表示样例布局与内容版本，不代表稳定的客户端线协议。
