# 官方例程

每个程序都是独立的 `.cpp`，包含完整入口和所需配置。页面中的代码直接包含仓库源码，构建时更新，无需手动同步。

## 按场景学习

| 顺序 | 程序 | 学习目标 | 运行行为 |
| --- | --- | --- | --- |
| 0 | `quickstart` | 任务、sleep、join、block_on | 打印 42 后退出 |
| 1 | [coroutine_basics](coroutines.md) | 等待、并发、后台任务、汇合、竞速 | 自动退出 |
| 2 | [coroutine_sync](synchronization.md) | 六类协程同步原语 | 自动退出 |
| 3 | [tcp_echo_server](network.md) | 每连接一个协程的 TCP 回显 | 监听 8080，常驻 |
| 4 | [udp_echo_server](network.md#udp-回显服务) | 数据报收发 | 监听 9090，常驻 |
| 5 | [tcp_counter_server](counter.md) | 分帧、共享状态、超时、错误 | 监听 8081，或 `--self-test` |
| 6 | [blocking_thread_pool](synchronization.md#阻塞函数与异步任务协作) | 同步 SDK 与异步任务并行 | 自动退出 |
| 7 | [tcp_echo_server_single_thread](network.md#单线程版本) | 单线程事件循环 | 监听 8080，常驻 |
| 8 | [file_and_directory](files.md) | 文件、目录、工作路径 | 自动退出，保留生成的文件 |

Windows 还有 `faio_windows_framework` 专用 IOCP 验证程序。C++26 实验层单独构建，见 [实验性例程](experimental.md)。

## 一次构建所有基础例程

先完成 [快速开始](../getting-started.md) 中 `build/local` 的配置，再执行：

```sh
cmake --build build/local --target quickstart coroutine_basics coroutine_sync \
  tcp_echo_server udp_echo_server tcp_counter_server blocking_thread_pool \
  tcp_echo_server_single_thread file_and_directory -j4
```

Windows 将构建目录替换为 `build/windows-msvc`，程序加 `.exe` 后缀。

Linux 各例程支持 `--io-backend=epoll|uring` 或环境变量 `FAIO_TEST_IO_BACKEND`，仅用于请求 / 校验已经编译的后端，不能覆盖编译时选择。`quickstart` 使用默认配置，无额外参数。

## 从源码继续探索

[完整示例 README](../../examples/README.md) 包含更多输出说明、协议交互与测试步骤。需要查接口时进入 [API 参考](../api/index.md)，需要理解实现时进入 [模块设计](../modules.md)。
