# faio · Fast Async IO

**简体中文** | [English](README_EN.md)

faio 是一个基于现代 C++ 编写的 Linux 异步 I/O 库：以 C++20 协程为执行模型，以 Linux io_uring 为原生 Proactor 异步引擎，提供协程任务、多线程运行时、网络与文件 I/O、定时器和协程同步原语。库以头文件形式提供，采用 [Apache-2.0](LICENSE) 开源许可证。

在 faio 中，异步流程以同步的方式书写：等待 I/O 完成、定时器到期或同步原语就绪时，当前协程挂起并让出工作线程；完成事件到达后，协程由运行时重新调度恢复。整个过程中没有线程被阻塞在读写或等待上，适用于高并发网络服务、异步任务处理和通用协程并发程序。

[功能](#功能) · [特点](#特点) · [简单示例](#简单示例) · [环境](#环境) · [集成](#集成) · [文档](#文档)

## 功能

| 模块 | 能力 |
| --- | --- |
| 协程任务 | 惰性 `task<T>`、任务启动与结果句柄、异常传播、协作取消 |
| 并发组合 | `join`、`join_all`、`select`、结构化任务作用域 `scope` |
| 异步运行时 | 默认运行时、独立运行时、多工作线程调度、任务窃取 |
| 网络 I/O | TCP 监听与连接、流读写、UDP 数据报、IPv4 / IPv6 地址 |
| 底层 I/O | 基于 io_uring 的文件与套接字操作 awaitable |
| 时间操作 | 休眠、截止时间、I/O 超时、周期性定时器 |
| 同步原语 | 互斥锁、信号量、条件变量、闩、屏障、有界 MPSC 队列 |
| 日志 | spdlog，支持运行时级别调整与输出 sink 配置 |

## 特点

- **C++20 无栈协程**：任务模型构建于 C++20 无栈协程（stackless coroutine）之上。`task<T>` 是惰性、移动独占的协程任务；嵌套 `co_await` 通过对称转移（symmetric transfer）直接接续子任务，不引入额外的栈分配与调度开销。结果与异常沿等待链逐层传播，异步错误可以用标准 `try/catch` 处理。
- **Proactor I/O 模型**：io_uring 负责异步请求提交与完成通知，运行时将完成事件转成协程就绪任务。
- **Worker-Thread 模式**：运行时由固定数量的工作线程组成，每个工作线程运行独立的事件循环，在同一循环中执行就绪协程、收割 io_uring 完成、处理定时器到期与跨线程唤醒。协程挂起即让出线程，任何任务都无法阻塞其所在的工作线程。
- **任务窃取调度**：采用本地就绪队列加全局队列的两级结构。同线程提交直接进入本地队列的快速槽，跨线程提交进入全局队列；空闲工作线程按窃取协议从其他线程的本地队列批量领取任务，在降低锁竞争的同时保持多核负载均衡。
- **协程语义的同步原语**：互斥锁、信号量、条件变量、闩、屏障与有界 MPSC 队列均为协程级原语——发生争用时挂起当前协程而非阻塞线程，恢复仍投递回原调度域；全部等待操作支持基于停止令牌的协作取消。
- **协程化的异步网络与文件 I/O**：TCP 监听与连接、流式读写、UDP 数据报，以及文件与套接字的底层操作，统一封装为可 `co_await` 的 awaitable。网络服务可以用同步的书写方式实现一连接一协程，并天然获得超时与协作取消能力。

## 简单示例

### 协程任务：block_on、spawn 与 join

```cpp
#include <faio/faio.hpp>
#include <chrono>

using namespace std::chrono_literals;

faio::task<int> delayed_value(int value) {
    co_await faio::time::sleep(10ms);
    co_return value;
}

faio::task<int> sum() {
    // spawn 立即提交任务并发执行，返回可等待的结果句柄
    auto first = faio::spawn(delayed_value(20));
    const auto a = co_await first;

    // join 把多个惰性任务组合为一个并发组合，按参数顺序返回结果
    auto [b, c] = co_await faio::join(delayed_value(11), delayed_value(11));
    co_return a + b + c;
}

int main() {
    // 普通线程用 block_on 等待任务完成；协程内部用 co_await
    const auto result = faio::block_on(sum());
    faio::log::logger()->info("sum = {}", result); // 42
    faio::runtime::shutdown();
}
```

### 协程同步：互斥锁与条件变量

```cpp
#include <faio/faio.hpp>
#include <chrono>

faio::sync::mutex g_mutex;
faio::sync::condition_variable g_cv;
bool g_ready = false;

faio::task<void> waiter() {
    co_await g_mutex.lock();
    // 等待期间挂起协程并释放互斥锁，通知到达后重新竞争锁
    co_await g_cv.wait(g_mutex, [] { return g_ready; });
    g_mutex.unlock();
    faio::log::logger()->info("条件满足，waiter 退出");
}

faio::task<void> notifier() {
    co_await faio::time::sleep(std::chrono::milliseconds{50});
    co_await g_mutex.lock();
    g_ready = true;
    g_mutex.unlock();
    g_cv.notify_all();
}

int main() {
    faio::block_on(faio::join(waiter(), notifier()));
    faio::runtime::shutdown();
}
```

### TCP 服务

```cpp
#include <faio/faio.hpp>
#include <cstdint>

faio::task<void> echo(faio::net::TcpStream stream) {
    char buf[1024];
    while (true) {
        auto n = co_await stream.read(buf);
        if (!n || *n == 0) break;
        if (!(co_await stream.write_all({buf, *n}))) break;
    }
}

faio::task<void> server(std::uint16_t port) {
    auto addr = faio::net::address::parse("0.0.0.0", port);
    auto listener = faio::net::TcpListener::bind(*addr);
    if (!listener) co_return;

    while (true) {
        auto accepted = co_await listener->accept();
        if (!accepted) break;
        auto& [stream, peer] = *accepted;
        faio::spawn_detached(echo(std::move(stream))); // 每个连接一个协程
    }
}

int main() {
    faio::block_on(server(8080));
}
```

默认运行时在首次使用时启动，可用 `faio::runtime::configure()` 在首次使用前调整线程数等参数。更多完整程序见 [examples](examples)，包括 [协程与并发](examples/coroutine_task.cpp)、[同步原语](examples/sync_demo.cpp)、[TCP 服务](examples/tcp_server.cpp) 和 [UDP 服务](examples/udp_server.cpp)。

日志默认以 `info` 级别输出到 stderr，包含时间、级别和线程 ID；级别与输出配置见 [运行时日志说明](docs/异步运行时.md#6-诊断日志)。

## 环境

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Linux，内核需支持使用到的 io_uring 操作；容器同样依赖宿主内核 |
| 编译器与标准库 | 支持 C++23，包括协程、`std::expected` 和 `std::format`；已验证 GCC 15 |
| 构建工具 | CMake 3.20+、pkg-config；预设使用 Ninja |
| 库依赖 | liburing、spdlog、线程库 |
| 可选开发依赖 | 单元测试使用 GoogleTest；TCP 性能对照使用 standalone Asio |
| 可选性能工具 | Rust / Cargo、wrk、Python 3 及 numpy / pandas / matplotlib，详见 [benchmark](benchmark/README.md) |

## 集成

### CMake 源码集成

将仓库作为子目录或 Git submodule 引入，通过 `faio::faio` 使用库：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)

set(FAIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/faio)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE faio::faio)
```

该目标自动传递 C++23 编译要求、头文件路径，以及 liburing、spdlog 和线程库链接依赖。应用中包含 `<faio/faio.hpp>` 即可使用完整库。

### CMake 安装集成

只安装库时，关闭示例、测试和性能测试，再指定安装目录：

```bash
cmake -S . -B build/install -G Ninja \
  -DFAIO_BUILD_EXAMPLES=OFF -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF \
  -DCMAKE_INSTALL_PREFIX=/path/to/faio-install
cmake --install build/install
```

安装内容包含头文件、许可证和 CMake 包。应用通过安装前缀查找包，并使用同一目标链接依赖：

```cmake
find_package(faio CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE faio::faio)
```

配置应用时传入 `-DCMAKE_PREFIX_PATH=/path/to/faio-install`，并确保 liburing、spdlog 与 pkg-config 可用。

### 构建示例与测试

在具备 C++23 编译器的 Linux 环境中，使用系统包安装依赖。以下命令以 Ubuntu 开发环境为例：

```bash
sudo apt-get install g++ cmake ninja-build pkg-config liburing-dev libspdlog-dev

git clone https://github.com/superlxh02/faio.git
cd faio
cmake --preset release -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset release -j4
./build/examples/coroutine_task
```

三个构建开关默认均为 `ON`。启用完整开发构建时，再安装 GoogleTest 和 standalone Asio：

```bash
sudo apt-get install libgtest-dev libasio-dev
cmake --preset debug
cmake --build --preset debug -j4
ctest --preset debug
```

`CMakePresets.json` 提供 `debug`、`release` 及对应的 vcpkg 预设。使用 vcpkg 时设置 `VCPKG_ROOT`，再选择 `vcpkg-debug` 或 `vcpkg-release`；清单包含库与开发构建的依赖。

## 文档

模块设计与源码解析：

| 文档 | 内容 |
| --- | --- |
| [协程封装](docs/协程封装.md) | 任务帧协议、任务上下文、this_coro、等待节点与调度边界 |
| [协程并发](docs/协程并发.md) | spawn / block_on、join_handle、join / select / scope 的完成边界 |
| [协程同步](docs/协程同步.md) | 等待节点协议、互斥锁、条件变量、信号量、闩、屏障与 MPSC |
| [异步运行时](docs/异步运行时.md) | 运行时组件、协程调度、I/O 驱动、启动与关闭流程 |
| [异步 I/O](docs/异步IO.md) | io_uring 封装与 I/O awaitable 的提交、完成与超时 |
| [网络 I/O](docs/网络IO.md) | TCP / UDP 接口的 Mixin 与 CRTP 设计及关键源码 |
| [定时器](docs/定时器.md) | 多级时间轮、休眠、周期 tick 与 I/O 超时 |

性能测试与复现方式见 [benchmark README](benchmark/README.md)。

## 许可证

[Apache-2.0](LICENSE)
