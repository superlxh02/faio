# 运行时与配置

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__runtime.html)

入口：`<faio/faio.hpp>`。默认运行时在首次使用时启动。配置发生在首次 `block_on()` / `spawn()` 之前。

## 最小配置

```cpp
faio::runtime::configure(
    faio::config_builder{}.set_num_workers(2).build());
```

`config_builder::build()` 返回配置副本，修改 builder 不会影响已启动的运行时。

## 常用接口

| 接口 | 用途 / 约束 |
| --- | --- |
| `runtime::configure(config)` | 配置默认运行时；已启动或已关闭时拒绝重新配置 |
| `runtime::shutdown(policy)` | 停止接纳并按策略排空；默认 `io::shutdown_policy::drain` |
| `config_builder::set_mode(mode)` | 选择 `multi_thread` 或 `current_thread` |
| `set_num_workers(count)` | 多线程模式的 worker 数量 |
| `set_io_backend(backend)` | Linux 校验构建时固定的 epoll / io_uring |
| `set_max_blocking_threads(count)` | 用户阻塞函数的线程池上限 |
| `set_blocking_queue_limit(count)` | 阻塞任务队列容量 |
| `set_filesystem_threads(count)` / `set_filesystem_queue_limit(count)` | 独立文件服务的线程与队列容量 |
| `set_resolver_threads(count)` / `set_resolver_queue_limit(count)` | 独立 DNS 服务的线程与队列容量 |

其他公平性、事件批量与轮询参数见 [config_builder 声明](../../include/faio/faio.hpp)。构建器的 setter 不表示参数一定有效；最终验证在运行时配置 / 构造入口执行。

## 单线程模式

```cpp
faio::runtime::configure(faio::config_builder{}
    .set_mode(faio::runtime::mode::current_thread)
    .set_num_workers(1)
    .build());
```

`current_thread` 模式由调用 `block_on()` 的线程驱动，不创建后台协程 worker。没有驱动时，队列、定时器和在途 I/O 保留到下一次驱动。独立提交的后台任务不会延长当前 `block_on()` 的等待。

用户阻塞函数仍由独立阻塞池执行，单线程调度不意味着整个进程只有一个线程。完整程序见 [单线程 TCP 例程](../examples/network.md#单线程版本)。

## 关闭约定

关闭前要让常驻任务有退出路径。默认 drain 不会替应用发明服务停止条件；仍在监听的无限循环可能持续等待。

不要在运行时 worker 上调用 `configure()` 或 `shutdown()`。默认运行时关闭之后不能重新启动；在普通线程上完成所需等待，再关闭运行时。

## 日志

`faio::log::logger()` 返回 spdlog logger，默认 `info` 级别，输出到 stderr，包含时间、级别和线程 ID。配置示例见 [错误与日志](errors.md#日志)。
