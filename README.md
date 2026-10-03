# faio · Fast Async IO

**简体中文** | [English](README_EN.md)

faio 是一个基于 C++20 协程构建的高性能跨平台异步 I/O 库。平台覆盖 Linux（io_uring/epoll 双后端）、macOS（kqueue）与 Windows（原生 IOCP 框架）；功能涵盖协程任务与同步原语、单/多线程运行时、TCP/UDP/Unix 网络、定时器，以及原生异步文件 I/O（无原生支持的操作由独立有界服务回退执行）。库以头文件形式提供，采用 [Apache-2.0](LICENSE) 开源许可证。

在 faio 中，异步流程以同步方式书写：协程等待 I/O、定时器或同步原语时挂起并让出工作线程，完成事件到达后由运行时恢复。uring 支持的文件操作提交原生 SQE，其余由独立有界服务执行，worker 全程不阻塞，适用于高并发网络服务、异步任务处理与通用协程并发程序。

[功能](#功能) · [特点](#特点) · [简单示例](#简单示例) · [环境](#环境) · [集成](#集成) · [文档](#文档)

## 功能

| 模块 | 能力 |
| --- | --- |
| 协程任务 | 惰性 `task<T>`、任务启动与结果句柄、异常传播、协作取消 |
| 并发组合 | `join`、`join_all`、`select`、结构化任务作用域 `scope` |
| 异步运行时 | 默认运行时、独立运行时、多工作线程调度、任务窃取 |
| 网络 I/O | TCP/UDP/Unix、异步 DNS、IPv4/IPv6、readiness、批量/聚集 IO、split、组播及原生扩展 |
| 底层 I/O | 中立完成协议、epoll/kqueue、AsyncFd、File/目录服务、有界缓冲与组合算法 |
| 时间操作 | 休眠、截止时间、I/O 超时、周期性定时器 |
| 同步原语 | 互斥锁、信号量、条件变量、闩、屏障、有界 MPSC 队列 |
| 日志 | spdlog，支持运行时级别调整与输出 sink 配置 |

## 特点

- **C++20 无栈协程**：`task<T>` 为惰性、移动独占的协程任务，嵌套 `co_await` 经对称转移（symmetric transfer）直接接续子任务，无额外栈分配与调度开销；结果与异常沿等待链传播，异步错误可用标准 `try/catch` 处理。
- **Proactor 模型**：应用提交完整 I/O 操作，后端执行完成时投递事件唤醒协程。io_uring 为原生 proactor；epoll/kqueue 与 Windows IOCP 经适配统一为同一 proactor 语义，应用代码无平台分支。
- **Worker-Thread 模式**：固定数量的工作线程各自运行独立事件循环，统一处理就绪协程、I/O 完成、定时器到期与跨线程唤醒。协程挂起即让出线程；阻塞工作使用 spawn_blocking，长计算主动让出协作预算。
- **任务窃取调度**：本地就绪队列加全局队列的两级结构，同线程提交进本地快速槽，跨线程提交进全局队列；空闲线程批量窃取任务，降低锁竞争并保持多核负载均衡。
- **协程语义的同步原语**：互斥锁、信号量、条件变量、闩、屏障与有界 MPSC 队列均为协程级原语，争用时挂起协程而非阻塞线程，恢复投递回原调度域；全部等待支持停止令牌协作取消。
- **协程化的异步网络与文件 I/O**：TCP/UDP 与文件、套接字底层操作统一封装为可 `co_await` 的 awaitable，以同步书写方式实现一连接一协程，并直接获得超时与协作取消能力。

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

默认运行时在首次使用时启动，可用 `faio::runtime::configure()` 在首次使用前调整线程数等参数。完整示例按学习场景组织：[协程基础与并发](examples/coroutine_basics.cpp)、[协程同步](examples/coroutine_sync.cpp)、[简单 TCP 服务](examples/tcp_echo_server.cpp)、[简单 UDP 服务](examples/udp_echo_server.cpp)、[TCP 共享计数器应用](examples/tcp_counter_server.cpp)、[阻塞线程池](examples/blocking_thread_pool.cpp)、[单线程 TCP 服务](examples/tcp_echo_server_single_thread.cpp) 和 [文件与目录操作](examples/file_and_directory.cpp)。详细中文讲解、协议和运行步骤见 [示例指南](examples/README.md)。

默认使用多线程模式，配置时无需调用 `set_mode()`。若希望由调用 `block_on` 的线程驱动协程，可在首次使用前设置 `faio::config_builder{}.set_mode(faio::runtime::mode::current_thread).build()` 并传给 `faio::runtime::configure()`，完整程序见 [单线程 TCP 示例](examples/tcp_echo_server_single_thread.cpp)。单线程模式不创建后台协程 worker；没有调用 `block_on` 时，提交的异步任务暂不推进，队列、定时器和在途 I/O 会保留到下一次驱动。`block_on` 等待本次任务组完成，独立提交的后台任务不会延长它的等待。`spawn_blocking` 仍使用独立的阻塞线程池，参见 [阻塞任务示例](examples/blocking_thread_pool.cpp)。调度取舍、性能实测和复现方法见 [单线程运行时性能](docs/单线程运行时性能.md)。

日志默认以 `info` 级别输出到 stderr，包含时间、级别和线程 ID；级别与输出配置见 [运行时日志说明](docs/异步运行时.md#6-诊断日志)。

## 环境

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Linux io_uring/epoll、macOS kqueue；Windows仅原生IOCP框架 |
| 编译器与标准库 | 支持 C++23，包括协程、`std::expected` 和 `std::format`；验证工具链为 Linux Clang 22.1.2、macOS Homebrew Clang 23（/opt/homebrew/opt/llvm） |
| 构建工具 | CMake 3.20+；预设使用 Ninja |
| 库依赖 | spdlog、线程库；Linux默认双后端需liburing，epoll-only构建不需要 |
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

该目标自动传递 C++23 编译要求、头文件路径，以及 spdlog 和线程库链接依赖。应用中包含 `<faio/faio.hpp>` 即可使用完整库。

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

配置应用时传入 `-DCMAKE_PREFIX_PATH=/path/to/faio-install`，并确保 spdlog 与线程库可用。

### 构建示例与测试

macOS 使用 `cmake --preset macos-clang23` 选择 Homebrew Clang 23，随后使用同名 build/test preset。Linux Clang 22 双后端使用 `linux-clang22-dual` configure/build preset，分别使用 `linux-clang22-dual-epoll` / `linux-clang22-dual-uring` 测试；无需 liburing 时使用 `linux-clang22-epoll`。非系统依赖安装目录通过 `CMAKE_PREFIX_PATH` 指定。

GCC 工具链需要包含[协程构造异常清理修复](https://github.com/gcc-mirror/gcc/commit/5422486d4bc728688dd2c874cb014fadf745b2da)。GCC 15.2.0-16ubuntu1 在帧内参数复制或移动抛异常时实测泄漏协程帧，不能作为完整异常安全的工具链。原 `linux-dual` / `linux-epoll` GCC 预设仍可用于包含此修复的编译器。

在具备 C++23 编译器的 Linux 环境中，使用系统包安装依赖。以下命令以 Ubuntu 开发环境为例：

```bash
sudo apt-get install g++ cmake ninja-build liburing-dev libspdlog-dev

git clone https://github.com/superlxh02/faio.git
cd faio
cmake --preset release -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset release -j4
./build/examples/coroutine_basics
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
| [异步 I/O](docs/异步IO.md) | uring 原生完成、epoll/kqueue、文件服务、缓冲与组合 IO、取消及生命周期 |
| [网络 I/O](docs/网络IO.md) | TCP / UDP 接口的 Mixin 与 CRTP 设计及关键源码 |
| [定时器](docs/定时器.md) | 多级时间轮、休眠、周期 tick 与 I/O 超时 |

性能测试与复现方式见 [benchmark README](benchmark/README.md)。

## 许可证

[Apache-2.0](LICENSE)
