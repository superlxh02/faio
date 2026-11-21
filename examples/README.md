# faio 可运行示例

每个程序围绕一个用户场景，只使用公开接口。程序通过 `faio::block_on` 等待异步任务，调度和 I/O 由库处理。所有非实验程序在演示完成后退出。

| 模块     | 源码                                                               | 构建目标                 | 展示内容                                           |
|----------|--------------------------------------------------------------------|--------------------------|----------------------------------------------------|
| 入门     | [quickstart.cpp](quickstart.cpp)                                   | `quickstart`             | 第一个异步任务，输出 42                            |
| 协程     | [task.cpp](coroutine/task.cpp)                                     | `coroutine_task`         | 配置默认运行时、惰性创建、等待任务和领取异常       |
| 并发上   | [composition_basics.cpp](coroutine/composition_basics.cpp)         | `composition_basics`     | 固定或动态汇合，`select` 等待落选工作退出          |
| 并发下   | [structured_concurrency.cpp](coroutine/structured_concurrency.cpp) | `structured_concurrency` | `scope` 自动收束、分离任务通知、共享结果与组内失败 |
| 协程     | [batch_results.cpp](coroutine/batch_results.cpp)                   | `batch_results`          | 单项终态、批量收集结果和限并发处理                 |
| 协程     | [race_strategies.cpp](coroutine/race_strategies.cpp)               | `race_strategies`        | 首个成功与任务组内的提前交付                       |
| 协程     | [lifecycle.cpp](coroutine/lifecycle.cpp)                           | `coroutine_lifecycle`    | 观察任务开始和结束，异步清理归还借用               |
| 协程     | [cancellation_deadline.cpp](coroutine/cancellation_deadline.cpp)   | `cancellation_deadline`  | 外部停止令牌、截止触发和落选分支收束               |
| 协程     | [sequence.cpp](coroutine/sequence.cpp)                             | `coroutine_sequence`     | 拉取、限并发消费、提前退出后等生产者收尾           |
| 协程     | [custom_awaiter.cpp](coroutine/custom_awaiter.cpp)                 | `custom_awaiter`         | 同步完成与外部线程回调接入                         |
| 协程     | [serial_domain.cpp](coroutine/serial_domain.cpp)                   | `serial_domain`          | 挂起期间保持独占，关闭后拒绝接纳并等待已有工作     |
| 阻塞工作 | [execution.cpp](runtime/execution.cpp)                             | `blocking_work`          | 通过 `spawn_blocking` 调用同步 SDK                 |
| 同步上   | [shared_state.cpp](sync/shared_state.cpp)                          | `sync_shared_state`      | 互斥锁、条件变量和信号量                           |
| 同步下   | [coordination.cpp](sync/coordination.cpp)                          | `sync_coordination`      | 闩、屏障和有界 MPSC 通道                           |
| I/O      | [stream.cpp](io/stream.cpp)                                        | `io_stream`              | 内存流、完整读写、缓冲及 EOF                       |
| 文件     | [file_and_directory.cpp](fs/file_and_directory.cpp)                | `file_and_directory`     | 写入、读回、枚举和清理临时目录                     |
| 网络     | [tcp_echo.cpp](net/tcp_echo.cpp)                                   | `tcp_echo`               | 本机 TCP 连接与字节流回显                          |
| 网络     | [udp_echo.cpp](net/udp_echo.cpp)                                   | `udp_echo`               | 本机 UDP 数据报与来源地址                          |
| TLS      | [echo.cpp](tls/echo.cpp)                                           | `tls_echo`               | 本机加密回显、显式 CA 和主机名验证                 |
| 进程     | [command.cpp](process/command.cpp)                                 | `process_command`        | 捕获标准输出及错误、超时后终止并回收               |

## 构建与运行

在仓库根目录执行。预设按 [快速开始](../docs/getting-started.md) 选择。macOS 示例：

```sh
cmake --preset mac-main-release
cmake --build --preset mac-main-release --target faio_examples
./build/mac-main-release/examples/quickstart
./build/mac-main-release/examples/structured_concurrency
./build/mac-main-release/examples/tcp_echo
ctest --preset mac-main-release -L example --output-on-failure
```

`faio_examples` 构建所有已启用的非实验示例，也可使用表中的名称单独构建。启用测试时，CTest 的 `example` 标签运行这一组程序。TCP、UDP 和 TLS 只使用本机回环，端口由系统分配。

TLS 需要 `FAIO_ENABLE_TLS=ON`。传入测试证书、私钥和 CA：

```sh
./build/mac-main-release/examples/tls_echo tests/fixtures/server.pem tests/fixtures/server.key tests/fixtures/ca.pem
```

这些证书和密钥用于本机测试。进程示例需要 `FAIO_ENABLE_PROCESS=ON`，请通过可执行文件的路径启动，示例会用同一路径启动子角色。

## 实验示例

每个程序以 `[[= faio::experimental::main(app)]]` 声明异步入口。应用实现 `faio::task<int> app(int, char**)`，库负责启动、退出收束和交付退出状态。

| 源码                                                            | 构建目标                           | 展示内容                                     |
|-----------------------------------------------------------------|------------------------------------|----------------------------------------------|
| [async_main.cpp](experimental/entry/async_main.cpp)             | `faio_experimental_async_main`     | 转交命令行参数，并发等待和进程退出码         |
| [exit_cleanup.cpp](experimental/entry/exit_cleanup.cpp)         | `faio_experimental_exit_cleanup`   | 入口返回、请求后台任务停止、等待异步清理     |
| [annotated_task.cpp](experimental/lifecycle/annotated_task.cpp) | `faio_experimental_annotated_task` | 惰性任务的前置钩子、成功和异常终态           |
| [sender_bridge.cpp](experimental/execution/sender_bridge.cpp)   | `faio_experimental_sender_bridge`  | 将协程接入 stdexec Sender 图，并异步领取结果 |

工具链要求 Linux、GCC 16.1.x、配套 libstdc++ 和 C++26。启用实验反射后，可构建全部已启用的示例。Sender 示例还要求启用 `FAIO_EXPERIMENTAL_EXECUTION` 并准备锁定的 stdexec，见 [实验性接口](../docs/api/experimental.md)。

```sh
cmake --preset linux-all-release
cmake --build --preset linux-all-release --target faio_experimental_examples
./build/linux-all-release/examples/experimental/faio_experimental_async_main faio
./build/linux-all-release/examples/experimental/faio_experimental_exit_cleanup
ctest --preset linux-all-release -L experimental -R '^example_' --output-on-failure
```
