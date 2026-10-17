# faio 公共 API 参考

面向库使用者的逐项接口文档。按模块查找类型与函数，点击方法名查看完整签名、重载、形参、返回值、错误及生命周期约定。

- @ref tasks "协程与并发"：task、spawn、join、select、scope 与任务结果句柄。
- @ref runtime "运行时与配置"：config_builder、运行模式、关闭策略与协程查询。
- @ref network "网络 I/O"：TCP、UDP、地址解析、完整传输和 socket 选项。
- @ref filesystem "文件与目录"：File、打开选项、元数据、目录枚举与路径操作。
- @ref time "时间与超时"：sleep、周期触发和 I/O deadline。
- @ref sync "协程同步"：互斥锁、信号量、条件变量、屏障和有界 MPSC。
- @ref io "缓冲与流算法"：缓冲区、读写适配、复制与 Unix 描述符。
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

`<faio/faio.hpp>` 是完整入口；网络、文件和流算法可分别使用 `<faio/net.hpp>`、`<faio/fs.hpp>`、`<faio/io.hpp>`。本参考按公共名称组织，包含公开别名继承的方法。Unix 描述符页仅适用于 Unix；Linux 专有选项会在不支持的平台返回错误或不提供，详见[平台说明](../book/platforms.html)。

[开始使用](../book/getting-started.html) · [官方例程](../book/examples/index.html) · [模块讲解](../book/modules.html)
