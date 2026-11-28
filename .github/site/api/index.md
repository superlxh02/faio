# faio 核心公共 API 参考

面向库使用者的核心公开接口查询。按下列模块查找已收录的类型与函数，点击方法名查看签名、重载、参数、返回值及生命周期约定。TLS、异步进程与实验层分别见[TLS介绍](../book/api/tls.html)、[进程介绍](../book/api/process.html)和[实验性接口介绍](../book/api/experimental.html)。

- @ref tasks "协程与并发"：task、spawn、join、select、task_group、共享结果与异步序列。
- @ref runtime "运行时与配置"：config_builder、运行模式、关闭策略与协程查询。
- @ref network "网络 I/O"：TCP、UDP、地址解析、完整传输和 socket 选项。
- @ref filesystem "文件与目录"：File、打开选项、元数据、目录枚举与路径操作。
- @ref time "时间与超时"：sleep、周期触发和 I/O deadline。
- @ref sync "协程同步"：互斥锁、信号量、条件变量、屏障和有界 MPSC。
- @ref io "缓冲与流算法"：缓冲区、读写适配、内存流、复制与 Unix 描述符。
- @ref errors "错误与日志"：expected、error 与日志入口。
- @ref match "模式匹配"：expected、optional、variant 与值的有序分支。
- @ref stdio "标准输入输出"：协程标准流与格式化输出。

## 如何阅读异步签名

返回 `task<T>` 的接口通过 `co_await` 取得 `T`。返回 `auto` 的 I/O 接口也通过 `co_await` 使用，其完成结果写在方法的“返回”说明中。先检查 `expected` 的成功状态，再访问结果；失败时使用 `error().message()` 和 `error().transferred()` 查询原因与部分进度。

```cpp
auto listener = faio::net::tcp_listener::bind(*address);
if (!listener) co_return;
auto client = co_await listener->accept();
if (!client) co_return;
auto& stream = client->first;
char bytes[4096];
auto count = co_await stream.read(bytes);
if (!count) co_return;
```

## 接入方式与平台

`<faio/faio.hpp>` 汇集已启用的稳定功能。应用也可以按需包含功能头，例如 `<faio/net/tcp.hpp>`、`<faio/fs/file.hpp>` 或 `<faio/io/stream.hpp>`。本参考按公共名称组织，包含公开别名继承的方法。Unix 描述符页仅适用于 Unix。Linux 专有选项会在不支持的平台返回错误或不提供，详见[快速开始](../book/getting-started.html)。

[faio 模块支持](../book/getting-started.html#faio-模块支持)通过 `import faio;` 提供相同的稳定 API，也可按功能导入 `faio.coroutine`、`faio.io`、`faio.net` 等模块。一个最终程序选择头文件或模块接入中的一种，工具链要求和已知限制见接入说明。

[快速开始](../book/getting-started.html) · [API 介绍](../book/api/index.html) · [API 查询](../book/api-reference.html)
