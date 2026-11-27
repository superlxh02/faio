# faio 示例

这里的每个程序都对应一个能跑起来的场景，只调用 faio 的公开接口。异步任务统一交给 `faio::block_on` 驱动，调度和 I/O 由库负责。除实验示例外，程序演示完就退出，输出可以直接看，也可以交给 CTest。

还没编译过 faio 的话，先按[快速开始](../docs/getting-started.md)选一个预设，再回到这里挑例子。

## 示例清单

先看两个入口程序。`quickstart` 用头文件接口建立异步任务，`quickstart_module` 只写 `import faio;`，并发等待两个任务得到 42。模块示例无需额外包含 `<coroutine>` 或导入 `std`。

| 源码 | CMake 目标 | 展示内容 |
|---|---|---|
| [quickstart.cpp](quickstart.cpp) | `quickstart` | 第一个异步任务，输出 `Hello, world!` |
| [module/quickstart.cpp](module/quickstart.cpp) | `quickstart_module` | 只导入 `faio`，并发任务返回 42，以退出码 0 检查结果 |

下面一组是协程和任务组合。它们覆盖任务生命周期、并发汇合、取消这些容易写错的地方。

| 源码 | CMake 目标 | 展示内容 |
|---|---|---|
| [coroutine/task.cpp](coroutine/task.cpp) | `coroutine_task` | 配置默认运行时、惰性创建、等待任务和领取异常 |
| [coroutine/composition_basics.cpp](coroutine/composition_basics.cpp) | `composition_basics` | 固定或动态汇合，`select` 等待落选工作退出 |
| [coroutine/structured_concurrency.cpp](coroutine/structured_concurrency.cpp) | `structured_concurrency` | `scope` 自动收束、分离任务通知、共享结果与组内失败 |
| [coroutine/batch_results.cpp](coroutine/batch_results.cpp) | `batch_results` | 单项终态、批量收集结果和限并发处理 |
| [coroutine/race_strategies.cpp](coroutine/race_strategies.cpp) | `race_strategies` | 首个成功与任务组内的提前交付 |
| [coroutine/lifecycle.cpp](coroutine/lifecycle.cpp) | `coroutine_lifecycle` | 观察任务开始和结束，异步清理归还借用 |
| [coroutine/cancellation_deadline.cpp](coroutine/cancellation_deadline.cpp) | `cancellation_deadline` | 外部停止令牌、截止触发和落选分支收束 |
| [coroutine/sequence.cpp](coroutine/sequence.cpp) | `coroutine_sequence` | 拉取、限并发消费、提前退出后等生产者收尾 |
| [coroutine/custom_awaiter.cpp](coroutine/custom_awaiter.cpp) | `custom_awaiter` | 同步完成与外部线程回调接入 |
| [coroutine/serial_domain.cpp](coroutine/serial_domain.cpp) | `serial_domain` | 挂起期间保持独占，关闭后拒绝接纳并等待已有工作 |

这一组涉及运行时、同步原语和 I/O 基础：从线程池承接阻塞调用，到锁、通道、流和文件系统。

| 源码 | CMake 目标 | 展示内容 |
|---|---|---|
| [runtime/execution.cpp](runtime/execution.cpp) | `blocking_work` | 通过 `spawn_blocking` 调用同步 SDK |
| [sync/shared_state.cpp](sync/shared_state.cpp) | `sync_shared_state` | 互斥锁、条件变量和信号量 |
| [sync/coordination.cpp](sync/coordination.cpp) | `sync_coordination` | 闩、屏障和有界 MPSC 通道 |
| [io/stream.cpp](io/stream.cpp) | `io_stream` | 内存流、完整读写、缓冲及 EOF |
| [fs/file_and_directory.cpp](fs/file_and_directory.cpp) | `file_and_directory` | 写入、读回、枚举和清理临时目录 |

网络和系统集成示例都只访问本机。TCP、UDP 和 TLS 使用回环地址，端口由系统分配，不会占用固定端口。

| 源码 | CMake 目标 | 展示内容 |
|---|---|---|
| [net/tcp_echo.cpp](net/tcp_echo.cpp) | `tcp_echo` | 持续监听的 TCP 回显服务（127.0.0.1:8080） |
| [net/udp_echo.cpp](net/udp_echo.cpp) | `udp_echo` | 持续监听的 UDP 回显服务（127.0.0.1:8081） |
| [tls/echo.cpp](tls/echo.cpp) | `tls_echo` | 本机加密回显、显式 CA 和主机名验证 |
| [process/command.cpp](process/command.cpp) | `process_command` | 捕获标准输出及错误、超时后终止并回收 |

表里写的是 CMake 目标名。Bazel 和 xmake 大部分沿用，只有少量前缀或别名不同：

- CMake 的 `quickstart` 在 xmake 里叫 `quickstart_header`；Bazel 仍叫 `quickstart`。
- `sync_shared_state` 和 `sync_coordination` 在 Bazel、xmake 中都叫 `shared_state`、`coordination`。
- xmake 的头文件版 TLS 与进程示例叫 `tls_echo_header`、`process_command_header`。
- 模块版示例统一用 `*_module` 后缀，例如 `tls_echo_module`、`process_command_module`。

## 构建与运行

### CMake

示例和库共用一次配置，可执行文件落在 `target/cmake-build-<preset>/examples/`。以 macOS 预设为例：

```sh
cmake --preset mac-main-release
cmake --build --preset mac-main-release --target faio_examples
./target/cmake-build-mac-main-release/examples/quickstart
./target/cmake-build-mac-main-release/examples/structured_concurrency
./target/cmake-build-mac-main-release/examples/tcp_echo
ctest --preset mac-main-release -L example --output-on-failure
```

`faio_examples` 聚合头文件版的非实验示例。也可以把表中的目标名直接交给 `cmake --build --preset <preset> --target <target>` 单独构建。开启 `FAIO_BUILD_TESTS` 后，每个程序注册成名为 `example_<target>` 的测试，带 `example` 标签；上面的 `ctest` 命令按标签把它们一起跑一遍。

TLS 和进程组件默认关闭。启用 `FAIO_ENABLE_TLS=ON` 后，`tls_echo` 需要依次传入服务端证书、私钥和客户端信任的 CA：

```sh
./target/cmake-build-mac-main-release/examples/tls_echo tests/fixtures/server.pem tests/fixtures/server.key tests/fixtures/ca.pem
```

这些文件只用于本机测试。进程示例在 `FAIO_ENABLE_PROCESS=ON` 后参与构建，直接运行即可，不要只传一个裸文件名，程序会用 `argv[0]` 定位自身来启动子进程：

```sh
./target/cmake-build-mac-main-release/examples/process_command
```

### Bazel

Bazel 版本锁在 9.2.0。头文件示例：

```sh
bazel run --config=faio_headers //examples:quickstart
bazel run --config=faio_headers //examples:structured_concurrency
bazel run --config=faio_headers //examples:tcp_echo
bazel run --config=faio_headers //examples:udp_echo
```

模块版组件示例：

```sh
bazel test --config=faio_modules --config=faio_tls --config=faio_process \
  //examples:tls_echo_module //examples:process_command_module
```

`quickstart_module` 使用同一套模块工具链：

```sh
bazel run --config=faio_modules //examples:quickstart_module
```

`examples` 包里的程序大多是 `cc_test`，所以 `bazel run` 和 `bazel test` 都能用；约定是失败时返回非零状态。TLS 和进程的头文件版目标也要带上对应配置：`--config=faio_tls`、`--config=faio_process`。`CC` 需要指向支持 C++23 的编译器，macOS 上可用的版本见[快速开始](../docs/getting-started.md)。

### xmake

使用 xmake 3.0.1 或更新版本。只跑头文件示例时保持 `--faio_modules=n`：

```sh
xmake f -c -y -m release --toolchain=clang \
  --cxx=/path/to/llvm/bin/clang++ --ld=/path/to/llvm/bin/clang++
xmake build quickstart_header
xmake run quickstart_header
xmake run coroutine_task
```

打开模块后，`tls_echo_module` 等 `*_module` 系列目标才存在：

```sh
xmake f -c -y -m release --toolchain=clang \
  --cxx=/path/to/llvm/bin/clang++ --ld=/path/to/llvm/bin/clang++ \
  --faio_modules=y --faio_tls=y --faio_process=y
xmake run tls_echo_module
```

配置后直接运行模块入门示例：

```sh
xmake run quickstart_module
```

xmake 默认把构建产物放在 `target/xmake/`。切换工具链或开关时记得加 `-c` 清掉旧配置。

## faio 模块支持示例

[`module/`](module/README.md) 是一个可以单独配置的下游 CMake 工程，演示怎样从源码或安装包接入 `faio::module`。程序只写 `import faio;`，并发等待两个任务得到 42，成功时返回退出码 0。它使用 `cxx-module/` 的模块接口和 `include/faio/` 的共享实现。

源码与安装接入的完整命令见[模块示例说明](module/README.md)。根目录构建打开 `FAIO_BUILD_MODULES=ON` 后也能直接构建 `quickstart_module`：

```sh
cmake -S . -B target/cmake-build-modules -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DFAIO_BUILD_MODULES=ON
cmake --build target/cmake-build-modules --target quickstart_module
./target/cmake-build-modules/tests/modules/quickstart_module
```

模块构建要求 CMake 3.28+、Ninja 1.11+ 和支持 C++23 模块的编译器。安装消费需要兼容的编译器、标准库和构建配置，由下游重建 BMI。功能模块、工具链约束和其他构建入口见[faio 模块支持](../cxx-module/README.md)。

## 实验示例

实验示例走另一套入口：程序用 `[[= faio::experimental::main(app)]]` 声明，应用实现 `faio::task<int> app(int, char**)`，启动、退出收束和退出状态交给库处理。

| 源码 | CMake 目标 | 展示内容 |
|---|---|---|
| [experimental/entry/async_main.cpp](experimental/entry/async_main.cpp) | `faio_experimental_async_main` | 转交命令行参数，并发等待和进程退出码 |
| [experimental/entry/exit_cleanup.cpp](experimental/entry/exit_cleanup.cpp) | `faio_experimental_exit_cleanup` | 入口返回、请求后台任务停止、等待异步清理 |
| [experimental/lifecycle/annotated_task.cpp](experimental/lifecycle/annotated_task.cpp) | `faio_experimental_annotated_task` | 惰性任务的前置钩子、成功和异常终态 |
| [experimental/execution/sender_bridge.cpp](experimental/execution/sender_bridge.cpp) | `faio_experimental_sender_bridge` | 将协程接入 stdexec Sender 图，并异步领取结果 |

这一组只支持 Linux、GCC 16.1.x、配套 libstdc++ 和 C++26。启用实验反射后构建：

```sh
cmake --preset linux-all-release
cmake --build --preset linux-all-release --target faio_experimental_examples
./target/cmake-build-linux-all-release/examples/experimental/faio_experimental_async_main faio
./target/cmake-build-linux-all-release/examples/experimental/faio_experimental_exit_cleanup
ctest --preset linux-all-release -L experimental -R '^example_' --output-on-failure
```

Sender 示例还要求开启 `FAIO_EXPERIMENTAL_EXECUTION`，并准备好锁定的 stdexec，细节见[实验性接口](../docs/api/experimental.md)。
