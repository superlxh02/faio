# faio · Fast Async IO

**简体中文** | [English](README_EN.md)

faio 是一个基于 C++20 协程构建的高性能跨平台异步 I/O 库。平台覆盖 Linux（io_uring/epoll 双后端）、macOS（kqueue）与 Windows（原生 IOCP/OVERLAPPED）；功能涵盖协程任务与同步原语、单/多线程运行时、TCP/UDP 网络、Unix 平台网络扩展、定时器，以及原生异步文件 I/O（无原生支持的操作由独立有界服务回退执行）。库以头文件形式提供，采用 [Apache-2.0](LICENSE) 开源许可证。

在 faio 中，异步流程以同步方式书写：协程等待 I/O、定时器或同步原语时挂起并让出工作线程，完成事件到达后由运行时恢复。uring 支持的文件操作提交原生 SQE，其余由独立有界服务执行，worker 全程不阻塞，适用于高并发网络服务、异步任务处理与通用协程并发程序。

[功能](#功能) · [特点](#特点) · [简单示例](#简单示例) · [环境](#环境) · [三方库集成](#三方库集成) · [源码编译](#源码编译) · [集成](#集成) · [文档](#文档)

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
| 实验性特性 | C++26 反射注解的异步入口与编译期运行时配置、stdexec Sender/Receiver 互操作（仅 Linux + GCC 16.1，独立构建开关，不影响基础库） |

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

### 实验性特性：注解入口与 Sender 互操作

```cpp
#include <faio/experimental/execution.h>
#include <faio/experimental/async_main.h>
#include <faio/faio.hpp>
#include <exec/task.hpp>
#include <chrono>

faio::task<int> native_job() {
    co_await faio::time::sleep(std::chrono::milliseconds{1});
    co_return 42;
}

exec::task<int> workflow(faio::experimental::runtime_ref runtime) {
    // exec::task 不传播 faio 查询，显式携带 runtime
    co_await stdexec::schedule(runtime.get_multi_thread_scheduler());
    co_return co_await runtime.as_sender(native_job());
}

// 注解声明入口与运行时配置，库生成真正的 main，无需手写 block_on
[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::multi_thread,
    .workers = 4,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> async_main(int, char**) {
    auto runtime = faio::experimental::this_runtime();
    auto result = runtime.spawn_sender(workflow(runtime)); // 提交 sender 图，返回可等待句柄
    const auto values = co_await std::move(result);
    co_return values && std::get<0>(*values) == 42 ? 0 : 1;
}
```

实验性特性要求 Linux + GCC 16.1 + C++26，经 `FAIO_ENABLE_EXPERIMENTAL` 及预设 `linux-gcc16.1-experimental-*` 构建；架构设计与源码解析见 [实验性特性](docs/实验性特性.md)，完整示例见 [examples/experimental](examples/experimental/)。

默认运行时在首次使用时启动，可用 `faio::runtime::configure()` 在首次使用前调整线程数等参数。完整示例按学习场景组织：[协程基础与并发](examples/coroutine_basics.cpp)、[协程同步](examples/coroutine_sync.cpp)、[简单 TCP 服务](examples/tcp_echo_server.cpp)、[简单 UDP 服务](examples/udp_echo_server.cpp)、[TCP 共享计数器应用](examples/tcp_counter_server.cpp)、[阻塞线程池](examples/blocking_thread_pool.cpp)、[单线程 TCP 服务](examples/tcp_echo_server_single_thread.cpp) 和 [文件与目录操作](examples/file_and_directory.cpp)。详细中文讲解、协议和运行步骤见 [示例指南](examples/README.md)。

默认使用多线程模式，配置时无需调用 `set_mode()`。若希望由调用 `block_on` 的线程驱动协程，可在首次使用前设置 `faio::config_builder{}.set_mode(faio::runtime::mode::current_thread).build()` 并传给 `faio::runtime::configure()`，完整程序见 [单线程 TCP 示例](examples/tcp_echo_server_single_thread.cpp)。单线程模式不创建后台协程 worker；没有调用 `block_on` 时，提交的异步任务暂不推进，队列、定时器和在途 I/O 会保留到下一次驱动。`block_on` 等待本次任务组完成，独立提交的后台任务不会延长它的等待。`spawn_blocking` 仍使用独立的阻塞线程池，参见 [阻塞任务示例](examples/blocking_thread_pool.cpp)。单线程模式的调度实现与多线程模式的差异见 [异步运行时](docs/异步运行时.md)。

日志默认以 `info` 级别输出到 stderr，包含时间、级别和线程 ID；级别与输出配置见 [运行时日志说明](docs/异步运行时.md#7-诊断日志)。

## 环境

| 项目 | 要求 |
| --- | --- |
| C++ 标准 | **C++23**：需要协程、`std::expected` 和 `std::format`；`faio::faio` 目标向消费方传递 `cxx_std_23`，CMake 配置期会实际编译检查这三项能力 |
| 编译器 — macOS | Homebrew LLVM **Clang ≥ 22**（`brew install llvm`，当前验证为 Clang 23，路径 `/opt/homebrew/opt/llvm`）；系统 AppleClang 的标准库不满足要求，配置期会给出明确错误 |
| 编译器 — Windows | **MSVC**（验证为 Visual Studio 18 的 MSVC 19.51）、**clang-cl**（验证为 LLVM Clang 22.1.0，使用 Microsoft STL 与 MSVC ABI）、**MinGW-w64**（验证为 MSYS2 UCRT64 GCC 16.2.0-4；必须 native TLS，emutls 工具链会在配置期被拒绝） |
| 编译器 — Linux | **Clang ≥ 22**（验证为 Ubuntu Clang 22.1.2，搭配 libstdc++ 15）或 **GCC**；GCC 必须包含[协程构造异常清理修复](https://github.com/gcc-mirror/gcc/commit/5422486d4bc728688dd2c874cb014fadf745b2da)——GCC 15.2.0-16ubuntu1 在帧内参数复制或移动抛异常时实测泄漏协程帧，不能作为完整异常安全的工具链 |
| 构建工具 | CMake 3.20+；预设使用 Ninja |
| 三方库 | spdlog（必需的公开链接依赖）、线程库；liburing（仅 Linux io_uring 后端需要，epoll-only 构建不需要） |
| 可选开发依赖 | 单元测试使用 GoogleTest；TCP 性能对照 benchmark 使用 standalone Asio |
| 可选性能工具 | Rust / Cargo、wrk、Python 3 及 numpy / pandas / matplotlib，详见 [benchmark](benchmark/README.md) |

平台 I/O 后端：Linux 为 io_uring/epoll 双后端（默认构建两者，运行 Linux 5.10 及更新内核时默认 io_uring），macOS 为 kqueue，Windows 为原生 IOCP/OVERLAPPED（构建目标 Windows 10/11 x64，导出 `_WIN32_WINNT=0x0A00`）。

## 三方库集成

faio 是头文件库，第三方依赖只在构建期需要；**CMake 配置期不联网下载**，依赖必须以下列方式之一预先提供：

| 方式 | 适用场景 | 做法 |
| --- | --- | --- |
| **vcpkg 清单** | 跨平台统一管理依赖 | 仓库根目录附带 `vcpkg.json` 清单（spdlog、gtest、asio、liburing[linux]）。`vcpkg install` 后配置时传入 `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` |
| **系统包 / 已安装包** | Linux、macOS 默认 | apt / brew 或自行编译安装的依赖经 `find_package(... CONFIG)` 查找；非标准安装前缀用 `-DCMAKE_PREFIX_PATH=...` 指定。对应 `FAIO_USE_WORKSPACE_DEPS=OFF`（非 Windows 默认值） |
| **固定版本源码** | Windows 默认 | `scripts/bootstrap_windows.ps1` 按 `scripts/windows-dependencies.json` 固定的版本、归档 URL 和 SHA256 下载并校验 spdlog 1.15.3、GoogleTest 1.17.0、Asio 1.36.0，解压到仓库相邻的 `faio-deps/`；CMake 经 `add_subdirectory` 用当前工具链重新编译（`FAIO_USE_WORKSPACE_DEPS=ON`，Windows 默认值），每个构建目录使用自己的编译器，避免混用 MSVC 与 MinGW 的 ABI |

补充说明：

- liburing 由 `cmake/FindLiburing.cmake` 查找，同时检查头文件、库及所需 API 的编译链接能力，并提供 `Liburing::Liburing` 导入目标。
- spdlog 是 `faio::faio` 的公开链接依赖（`target_link_libraries(faio INTERFACE ... spdlog::spdlog)`），消费方链接 `faio::faio` 后无需手工处理。
- GoogleTest 与 Asio 分别由测试和 benchmark 子目录按需查找，只构建库本身时不参与配置。
- 固定源码缓存位置可用 `-DFAIO_DEPENDENCY_ROOT=` 覆盖（默认为仓库相邻的 `faio-deps`）。

## 源码编译

四个构建开关 `FAIO_BUILD_EXAMPLES`、`FAIO_BUILD_TESTS`、`FAIO_BUILD_BENCHMARKS`、`FAIO_INSTALL` 默认均为 `ON`。`CMakePresets.json` 提供 Windows 的 `windows-msvc`、`windows-clang`、`windows-mingw`、`windows-clang-asan`，Linux 的 `linux-clang22-dual`、`linux-clang22-epoll`，以及 macOS 的 `macos-clang23`；除 Linux 双后端测试使用两个专用名称外，configure、build 和 test 均使用同名预设。自定义 Debug、Docker 或 vcpkg 构建可用 `cmake -S . -B <构建目录>` 显式指定参数。

### Windows（MSVC / clang-cl / MinGW-w64）

PowerShell 中先准备固定版本依赖与工具链，再选择编译器构建：

```powershell
./scripts/bootstrap_windows.ps1           # 首次：下载固定依赖与 MSYS2 UCRT64 MinGW 工具链（-Offline 验证离线缓存）
. ./scripts/windows_environment.ps1 msvc  # 或 clang / mingw
cmake --preset windows-msvc               # 或 windows-clang / windows-mingw
cmake --build --preset windows-msvc
ctest --preset windows-msvc
./build/windows-msvc/examples/coroutine_basics.exe
```

完整三编译器矩阵（构建、测试、安装包头文件独立编译与双翻译单元消费验证）使用 `./scripts/build_windows.ps1 -Offline`，日志保存到 `build/windows-matrix`。

MinGW 要点：工具链必须采用 native TLS（MSYS2 GCC 16 起），CMake 会实际生成汇编验证并拒绝 emutls；MinGW GCC 的[多翻译单元 TLS 缺陷](https://sourceforge.net/p/mingw-w64/bugs/994/)由仅 Windows GCC 启用的编译 launcher 在对象编译成功后精确局部化一个内部初始化 alias，该适配需要 Python 3 与 MinGW 的 nm/objcopy，且不支持 GCC LTO（项目 Release 默认关闭 LTO）。MinGW 可执行程序运行时需要 `faio-deps/tools/msys2-ucrt64/ucrt64/bin` 的同发行版 DLL，环境脚本已将该目录加入当前进程 PATH。

clang-cl 的 AddressSanitizer 验证使用独立预设，对 faio 消费目标、spdlog 和 GoogleTest 一起启用检测：

```powershell
. ./scripts/windows_environment.ps1 clang
cmake --preset windows-clang-asan
cmake --build --preset windows-clang-asan
ctest --preset windows-clang-asan
```

### Linux（Clang / GCC）

在已安装 Clang 22 及 C++23 标准库的 Linux 环境中，使用系统包安装依赖。以下命令以 Ubuntu 开发环境为例：

```bash
sudo apt-get install cmake ninja-build liburing-dev libspdlog-dev

git clone https://github.com/superlxh02/faio.git
cd faio
cmake --preset linux-clang22-dual
cmake --build --preset linux-clang22-dual -j4
./build/linux-clang22-dual/examples/coroutine_basics
```

双后端为同一个构建，分别用两个测试预设运行，无需重新编译：

```bash
ctest --preset linux-clang22-dual-epoll
ctest --preset linux-clang22-dual-uring
```

无需 liburing 时改用 `linux-clang22-epoll` 预设（测试预设同名）。启用完整开发构建（测试与 benchmark）需再安装 GoogleTest 和 standalone Asio：

```bash
sudo apt-get install libgtest-dev libasio-dev
```

GCC 工具链的版本要求见[环境](#环境)一节。

**Docker 一键构建**：不准备本地工具链时，可以用 Docker 一次完成编译与测试（构建上下文为仓库根目录）：

```bash
docker build -f docker/build.dockerfile -t faio:build .
docker run --rm --security-opt seccomp=unconfined faio:build \
  ctest --test-dir build --output-on-failure
```

`seccomp=unconfined` 允许测试和示例调用 io_uring；分层环境镜像、构建参数（`DEV_IMAGE`、`CMAKE_BUILD_TYPE`、`BUILD_JOBS`）与挂载工作目录开发的用法见 [docker/README.md](docker/README.md)。

### macOS（Homebrew Clang）

```bash
brew install llvm cmake ninja spdlog googletest

cmake --preset macos-clang23
cmake --build --preset macos-clang23 -j4
ctest --preset macos-clang23
./build/macos-clang23/examples/coroutine_basics
```

预设选择 `/opt/homebrew/opt/llvm/bin/clang++`，并以 `/opt/homebrew` 为依赖查找前缀；只构建库时可关闭测试与 benchmark，不安装 GoogleTest 和 Asio。

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

配置应用时传入 `-DCMAKE_PREFIX_PATH=/path/to/faio-install`，并确保 spdlog 与线程库可用。以源码编译方式构建本仓库的示例与测试见上文[源码编译](#源码编译)。

## 文档

模块设计与源码解析：

| 文档 | 内容 |
| --- | --- |
| [协程封装](docs/协程封装.md) | 任务帧协议、任务上下文、this_coro、等待节点与调度边界 |
| [协程并发](docs/协程并发.md) | spawn / block_on、join_handle、join / select / scope 的完成边界 |
| [协程同步](docs/协程同步.md) | 等待节点协议、互斥锁、条件变量、信号量、闩、屏障与 MPSC |
| [异步运行时](docs/异步运行时.md) | 运行时组件、协程调度、I/O 驱动、启动与关闭流程 |
| [异步 I/O](docs/异步IO.md) | uring 原生完成、epoll/kqueue、Windows IOCP 后端、文件服务、缓冲与组合 IO、取消及生命周期 |
| [网络 I/O](docs/网络IO.md) | TCP / UDP 接口的 Mixin 与 CRTP 设计及关键源码 |
| [定时器](docs/定时器.md) | 多级时间轮、休眠、周期 tick 与 I/O 超时 |
| [实验性特性](docs/实验性特性.md) | 注解异步入口、编译期运行时配置、stdexec Sender/Receiver 适配的架构与源码解析 |

性能测试与复现方式见 [benchmark README](benchmark/README.md)。

## 许可证

[Apache-2.0](LICENSE)
