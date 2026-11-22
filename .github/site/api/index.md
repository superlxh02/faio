# faio 公共 API 参考

通过搜索框输入类型、函数或成员名，也可以从 @ref public_surface "模块与头文件索引" 查找导入方式和声明。各专题列出相应的类型、重载和成员。

- @ref tasks "协程与并发"：task、spawn、join、select、scope 与任务结果句柄。
- @ref runtime "运行时与配置"：config_builder、运行模式、关闭策略与协程查询。
- @ref network "网络 I/O"：TCP、UDP、地址解析、完整传输和 socket 选项。
- @ref filesystem "文件与目录"：File、打开选项、元数据、目录枚举与路径操作。
- @ref time "时间与超时"：sleep、周期触发和 I/O deadline。
- @ref sync "协程同步"：互斥锁、信号量、条件变量、屏障和有界 MPSC。
- @ref io "缓冲与流算法"：缓冲区、读写适配、复制与 Unix 描述符。
- @ref tls "TLS 安全传输"：信任配置、服务端身份、握手与加密流。
- @ref process "异步进程"：命令、子进程、标准流及输出捕获。
- @ref experimental "实验性接口"：运行时选项、反射、异步入口与 Sender/Receiver。
- @ref errors "错误与日志"：expected、Error 与日志入口。

## 如何阅读异步签名

返回 `task<T>` 的接口通过 `co_await` 取得 `T`。返回 `auto` 的 I/O 接口也通过 `co_await` 使用，其完成结果写在方法的“返回”说明中。先检查 `expected` 的成功状态，再访问结果；失败时使用 `error().message()` 和 `error().transferred()` 查询原因与部分进度。

```cpp
auto listener = faio::net::TcpListener::bind(*address);
if (!listener) co_return;
auto client = co_await listener->accept();
if (!client) co_return;
auto& stream = client->first;
char bytes[4096];
auto count = co_await stream.read(bytes);
if (!count) co_return;
```

## 头文件与平台

`<faio/faio.hpp>` 汇集已启用的稳定功能。应用也可以按需包含功能头，例如 `<faio/net/tcp.hpp>`、`<faio/fs/file.hpp>` 或 `<faio/io/stream.hpp>`。本参考按公共名称组织，包含公开别名继承的方法。Unix 描述符页仅适用于 Unix。Linux 专有选项会在不支持的平台返回错误或不提供，详见[快速开始](../book/getting-started.html)。

[快速开始](../book/getting-started.html) · [API 介绍](../book/api/index.html) · [API 查询](../book/api-reference.html)
