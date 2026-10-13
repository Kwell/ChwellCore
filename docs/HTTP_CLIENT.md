# HTTP 客户端

`chwell::http::HttpClient` 是独立的可选 C++17 组件，支持 HTTP/HTTPS、有界二进制请求与响应、总调用期限及跨线程取消。它不依赖核心网络事件循环；阻塞调用应放在工作线程。

```bash
sudo apt-get install libcurl4-openssl-dev
cmake -S . -B build -DCHWELL_USE_HTTP_CLIENT=ON
cmake --build build --parallel 4
```

安装后消费：

```cmake
find_package(ChwellCore 0.1 CONFIG REQUIRED COMPONENTS http_client)
target_link_libraries(my_service PRIVATE Chwell::http_client)
```

```cpp
#include "chwell/http/http_client.h"

chwell::http::HttpClient client; // 在启动工作线程前构造
chwell::http::ClientRequest request;
request.method = "POST";
request.url = "https://api.example.com/events";
request.headers = {{"Content-Type", "application/json"}};
request.body = R"({"event":"login"})";
chwell::http::ClientOptions options;
options.connect_timeout_ms = 500;
options.request_timeout_ms = 1500;
options.max_response_bytes = 64 * 1024;
chwell::http::CancellationToken cancellation;
chwell::http::ClientResponse response;
auto result = client.request(request, response, options, cancellation);
if (result.ok()) {
    // 检查 response.status，再按业务规则处理 response.body。
}
```

另一个线程可调用 `cancellation.cancel()`；token 副本共享一次性取消标记，新意图应创建新 token。同一 client 可并发执行独立请求，各调用使用自己的输入和响应对象。调用期间不得修改输入、选项或响应对象。

| 结果 | 含义 |
| --- | --- |
| `Ok` | 收到完整 HTTP 响应，包括 3xx、4xx、5xx；检查 `response.status` |
| `Invalid` | URL、方法、请求头、请求大小或选项不合法，未发起网络请求 |
| `Cancelled` / `TimedOut` | 取消或耗尽总调用期限；远端可能已经执行写入 |
| `ResponseTooLarge` | 解码后的 body 或全部响应头超过配置限制 |
| `TlsError` | TLS 握手、证书或主机名校验失败 |
| `TransportError` | 连接、协议、截断响应或资源错误 |
| `Unsupported` | libcurl 不提供异步 DNS，无法执行有期限的解析 |

每种失败都保留原 `response`。`native_code` 在可用时提供 libcurl 错误码，不包含 URL、认证头或远端响应。取消/超时不证明服务端没有执行；客户端不重试，业务写入应自行使用持久化请求 ID 和结果恢复协议。

`request_timeout_ms` 从调用入口按 steady clock 计时，覆盖准备、DNS、连接、TLS、上传和完整接收；持续收到数据不会延长期限。连接预算取 `connect_timeout_ms` 与总剩余期限的较小值。取消通过进度回调和最多 50 ms 的 multi poll 检查，实际返回延迟仍受线程调度、TLS 后端和资源清理影响，不是硬实时保证。

默认连接期限 1 s、总期限 3 s、请求 body 1 MiB、响应 body 4 MiB、响应头 64 KiB。三个大小预算可设为 0，配置上限为 64 MiB。响应头预算包含状态行、临时 1xx 响应及 trailers；最终 `headers` 和 `trailers` 分开保存，小写名称且保留重复字段。curl 支持的压缩编码自动解码，body 预算作用于解码后的字节；实现会缓存完整响应，预算之外仍有 libcurl、TLS 和容器的内存开销。

支持 GET、HEAD、POST、PUT、PATCH、DELETE、OPTIONS，body 保留 NUL 等原始字节；GET/HEAD 不接受 body。URL 限 8192 字节，仅 HTTP/HTTPS，拒绝用户名密码、fragment、空白与控制字符。请求最多 64 个头，总计 64 KiB；禁止覆盖 Host、Content-Length、Transfer-Encoding、Connection、Proxy-Authorization、Expect，防止破坏传输边界。

HTTPS 始终校验证书和主机名，空 `ca_file` 使用 TLS 后端默认信任来源，非空值指定 PEM CA 文件；没有关闭校验的开关。当前使用 HTTP/1.1，每次独立连接，不提供连接池、自动重定向、代理或重试，忽略环境代理变量。URL 来源、目标访问权限、业务认证和凭据保管由应用负责。

需要 libcurl >=7.68 且启用异步 DNS（线程解析器或 c-ares），HTTPS 还需要 curl 的 TLS 后端。旧版 libcurl 没有线程安全全局初始化时，应在其他工作线程启动前构造 client；全局 curl 状态保留到进程退出，应用不得在请求或解析工作仍运行时调用 `curl_global_cleanup()`。`CHWELL_USE_CONSUL=ON` 自动构建并使用该组件；默认核心构建不会增加 CURL 依赖。框架的 `CHWELL_USE_OPENSSL` 控制核心 TLS，与 curl 自己的 TLS 后端独立。

真实 HTTP/TLS 契约测试使用 Python 3.8+ 与 OpenSSL CLI 临时生成测试证书，不安装到系统信任库：

```bash
cmake -S tests/http_client -B build-http-client
cmake --build build-http-client --parallel 4
ctest --test-dir build-http-client --output-on-failure
```

该独立测试支持 Linux 和 Windows/MSVC，不要求完整核心的 POSIX 模块。Windows CI 使用测试专用 `CHWELL_TEST_FETCH_CURL=ON` 构建固定版本 curl/Schannel；正常组件从系统或指定前缀发现 CURL，不自动下载。Linux CI 同时验证压缩后超限、ASan/UBSan、TSan，以及移动安装包后通过 `Chwell::http_client` 链接和执行这些契约。
