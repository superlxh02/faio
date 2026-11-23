# 运行时与配置

[查看此模块的 Doxygen 逐项接口：签名、形参、返回值与重载 →](../../api/group__runtime.html)

默认运行时的接口在 `<faio/runtime/default.hpp>`，独立运行时在 `<faio/runtime/instance.hpp>`。两者都提供 `config_builder`。默认运行时到第一次使用时才启动，配置必须在首次 `block_on()` 或 `spawn()` 之前完成。

## 接口

| 类别 | 接口 | 行为 |
| --- | --- | --- |
| 默认运行时 | `runtime::configure(config)` | 配置默认运行时；已启动或已关闭时拒绝 |
| | `runtime::shutdown(policy)` | 停止接纳并按策略排空，默认 `drain` |
| | `config_builder::build()` | 返回配置副本，之后修改 builder 不影响已启动运行时 |
| 配置 | `set_mode(mode)` | 选择 `multi_thread` 或 `current_thread` |
| | `set_num_workers(count)` | 多线程模式的 worker 数量 |
| | `set_no_stealing_queue_capacity(count)` | 每 worker 不可窃取任务与恢复槽容量，默认 256，启动时固定 |
| | `set_io_backend(backend)` | Linux 上选择构建时固定的 epoll 或 io_uring |
| | `set_max_blocking_threads(count)` | 用户阻塞函数的线程池上限 |
| | `set_blocking_pool_policy(policy)` | 用户阻塞池策略，默认 `elastic`，可选 `fixed` |
| | `set_blocking_keep_alive(duration)` | 弹性阻塞线程的空闲保活时长，默认 10 秒 |
| | `set_blocking_queue_limit(count)` | 阻塞任务队列容量 |
| | `set_filesystem_threads` / `set_filesystem_queue_limit` | 独立文件服务的线程与队列容量 |
| | `set_resolver_threads` / `set_resolver_queue_limit` | 独立 DNS 服务的线程与队列容量 |
| 运行模式 | `multi_thread` | 后台 worker 驱动任务 |
| | `current_thread` | 调用 `block_on()` 的线程驱动，不创建后台协程 worker |
| | `elastic` 阻塞池 | 从零线程开始按需创建，空闲到保活时长后退出 |
| | `fixed` 阻塞池 | 构造时预热到线程上限，线程保留到关闭 |
| 跨运行时 | `anchored_runtime` | 固定多线程模式并拥有独立驱动线程；析构时先关闭准入再排空 |
| | `anchored_runtime::anchor()` | 返回准入凭据，关闭后仍可保存 |
| | `on(anchor, task)` | 投到目标实例执行；两边静态调度器类型必须相同 |
| | `anchored_runtime::stop(policy)` | 关闭准入并按策略排空，不能从运行时 worker 同步调用 |
