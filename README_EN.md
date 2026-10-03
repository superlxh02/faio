# faio · Fast Async IO

[简体中文](README.md) | **English**

faio is a high-performance, cross-platform asynchronous I/O library built on C++20 coroutines (requiring a C++23 toolchain). It runs on Linux (dual io_uring/epoll backends), macOS (kqueue), and Windows (a native IOCP framework), and provides coroutine tasks and synchronization primitives, single/multi-thread runtimes, TCP/UDP/Unix networking, timers, and native asynchronous file I/O (operations without native support fall back to an isolated bounded service). The library is header-only and licensed under [Apache-2.0](LICENSE).

With faio, asynchronous flows read like synchronous code: a coroutine suspends and yields its worker thread while awaiting I/O, a timer, or a synchronization primitive, and the runtime resumes it on completion. io_uring-capable file operations are submitted as native SQEs and the rest run on an isolated bounded service, so workers never block on I/O—suited to high-concurrency network services, asynchronous task processing, and general coroutine-based concurrent programs.

[Features](#features) · [Highlights](#highlights) · [Examples](#examples) · [Requirements](#requirements) · [Integration](#integration) · [Documentation](#documentation)

## Features

| Module | Capabilities |
| --- | --- |
| Coroutine tasks | Lazy `task<T>`, task launching and result handles, exception propagation, cooperative cancellation |
| Concurrency combinators | `join`, `join_all`, `select`, structured task scope `scope` |
| Async runtime | Default runtime, standalone runtimes, multi-worker scheduling, work stealing |
| Network I/O | TCP/UDP/Unix, asynchronous DNS, IPv4/IPv6, readiness, batched/vectored I/O, split, multicast, and native extensions |
| Low-level I/O | Neutral completion protocol, epoll/kqueue, AsyncFd, File/directory services, bounded buffers and composite algorithms |
| Time operations | Sleep, deadlines, I/O timeouts, periodic timers |
| Synchronization primitives | Mutex, semaphore, condition variable, latch, barrier, bounded MPSC queue |
| Logging | spdlog with runtime level adjustment and sink configuration |

## Highlights

- **C++20 stackless coroutines**: `task<T>` is a lazy, move-only coroutine task; nested `co_await` continues into the child through symmetric transfer with no extra stack allocation or scheduling overhead. Results and exceptions propagate along the await chain and are handled with standard `try/catch`.
- **Proactor model**: the application submits a complete I/O operation, and the backend posts a completion event that resumes the coroutine. io_uring is a native proactor; epoll/kqueue and Windows IOCP are adapted to the same proactor semantics, so application code needs no per-platform branches.
- **Worker-thread model**: a fixed set of worker threads each runs an independent event loop handling ready coroutines, I/O completions, timer expirations, and cross-thread wakeups. A suspended coroutine yields its thread; blocking work uses spawn_blocking and long computations yield cooperatively.
- **Work-stealing scheduling**: a two-level structure of per-worker local queues plus a global queue—same-thread submissions enter a local fast slot, cross-thread submissions the global queue; idle workers steal tasks in batches, reducing lock contention while balancing load across cores.
- **Coroutine-semantics synchronization primitives**: mutex, semaphore, condition variable, latch, barrier, and bounded MPSC queue suspend the coroutine instead of blocking the thread on contention, with resumption delivered back to the original scheduling domain; all waits support stop-token cooperative cancellation.
- **Coroutinized async network and file I/O**: TCP/UDP and low-level file/socket operations are uniformly `co_await`-able; write one coroutine per connection in a synchronous style, with timeout and cooperative cancellation built in.

## Examples

### Coroutine tasks: block_on, spawn and join

```cpp
#include <faio/faio.hpp>
#include <chrono>

using namespace std::chrono_literals;

faio::task<int> delayed_value(int value) {
    co_await faio::time::sleep(10ms);
    co_return value;
}

faio::task<int> sum() {
    // spawn submits the task immediately and returns an awaitable result handle
    auto first = faio::spawn(delayed_value(20));
    const auto a = co_await first;

    // join combines lazy tasks into one concurrent composition,
    // returning results in argument order
    auto [b, c] = co_await faio::join(delayed_value(11), delayed_value(11));
    co_return a + b + c;
}

int main() {
    // Use block_on from a regular thread; use co_await inside coroutines
    const auto result = faio::block_on(sum());
    faio::log::logger()->info("sum = {}", result); // 42
    faio::runtime::shutdown();
}
```

### Coroutine synchronization: mutex and condition variable

```cpp
#include <faio/faio.hpp>
#include <chrono>

faio::sync::mutex g_mutex;
faio::sync::condition_variable g_cv;
bool g_ready = false;

faio::task<void> waiter() {
    co_await g_mutex.lock();
    // The coroutine suspends and releases the mutex while waiting,
    // then re-acquires it once notified
    co_await g_cv.wait(g_mutex, [] { return g_ready; });
    g_mutex.unlock();
    faio::log::logger()->info("condition met, waiter exits");
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

### TCP server

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
        faio::spawn_detached(echo(std::move(stream))); // one coroutine per connection
    }
}

int main() {
    faio::block_on(server(8080));
}
```

The default runtime starts on first use; call `faio::runtime::configure()` beforehand to adjust worker count and other parameters. The examples cover [coroutine basics and concurrency](examples/coroutine_basics.cpp), [coroutine synchronization](examples/coroutine_sync.cpp), [TCP echo](examples/tcp_echo_server.cpp), [UDP echo](examples/udp_echo_server.cpp), a [shared TCP counter application](examples/tcp_counter_server.cpp), [blocking work alongside async tasks](examples/blocking_thread_pool.cpp), [TCP echo with current-thread scheduling](examples/tcp_echo_server_single_thread.cpp), and [file and directory operations](examples/file_and_directory.cpp). See the [example guide](examples/README.md) for detailed Chinese explanations and commands; `tcp_counter_server --self-test` runs a finite multi-client scenario.

The default is `multi_thread`; calling `set_mode()` is optional. To drive coroutines on the thread calling `block_on`, configure `faio::config_builder{}.set_mode(faio::runtime::mode::current_thread).build()` before first use; see the [current-thread TCP example](examples/tcp_echo_server_single_thread.cpp). This mode starts no background async worker. Queues, timers and in-flight I/O persist between calls, and submitted async tasks advance while `block_on` runs. `block_on` waits for its task group, without waiting for independently submitted background roots. `spawn_blocking` still uses a separate blocking pool; see the [blocking task example](examples/blocking_thread_pool.cpp) and [scheduling and performance report](docs/单线程运行时性能.md).

Logs are written to stderr at `info` level by default, with timestamps, levels, and thread IDs. See [runtime logging](docs/异步运行时.md#6-诊断日志) for configuration (documentation is currently available in Chinese).

## Requirements

| Item | Requirement |
| --- | --- |
| Operating system | Linux io_uring/epoll, macOS kqueue; Windows IOCP framework only |
| Compiler and standard library | C++23 support including coroutines, `std::expected`, and `std::format`; validation toolchains are Linux Clang 22.1.2 and macOS Homebrew Clang 23 |
| Build tools | CMake 3.20+, pkg-config; presets use Ninja |
| Library dependencies | spdlog and threads; Linux dual-backend builds require liburing, epoll-only builds do not |
| Optional dev dependencies | GoogleTest for unit tests; standalone Asio for TCP benchmark comparison |
| Optional benchmark tools | Rust / Cargo, wrk, Python 3 with numpy / pandas / matplotlib — see [benchmark](benchmark/README.md) |

## Integration

### CMake source integration

Add the repository as a subdirectory or Git submodule, then link against `faio::faio`:

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

The target automatically propagates the C++23 requirement, include paths, and the spdlog / threads link dependencies. Include `<faio/faio.hpp>` in your application to use the full library.

### CMake install integration

To install only the library, disable examples, tests, and benchmarks, then choose an install prefix:

```bash
cmake -S . -B build/install -G Ninja \
  -DFAIO_BUILD_EXAMPLES=OFF -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF \
  -DCMAKE_INSTALL_PREFIX=/path/to/faio-install
cmake --install build/install
```

The install tree contains headers, the license, and CMake package files. Applications locate the package via the install prefix and link the same target:

```cmake
find_package(faio CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE faio::faio)
```

Pass `-DCMAKE_PREFIX_PATH=/path/to/faio-install` when configuring the application, and make sure spdlog and threads are available.

### Building examples and tests

On macOS, use the `macos-clang23` configure/build/test presets for Homebrew Clang 23. On Linux with Clang 22, use `linux-clang22-dual` to configure/build and `linux-clang22-dual-epoll` / `linux-clang22-dual-uring` to test; use `linux-clang22-epoll` without liburing. Set `CMAKE_PREFIX_PATH` for dependencies installed outside the system prefix.

GCC must include the [coroutine constructor exception cleanup fix](https://github.com/gcc-mirror/gcc/commit/5422486d4bc728688dd2c874cb014fadf745b2da). GCC 15.2.0-16ubuntu1 leaks the coroutine frame when a frame parameter copy or move throws and does not satisfy the complete exception safety requirement. The existing GCC `linux-dual` / `linux-epoll` presets remain available for compilers containing this fix.

On a Linux system with a C++23 compiler, install dependencies from system packages. The following commands use an Ubuntu development environment:

```bash
sudo apt-get install g++ cmake ninja-build liburing-dev libspdlog-dev

git clone https://github.com/superlxh02/faio.git
cd faio
cmake --preset release -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset release -j4
./build/examples/coroutine_basics
```

All three build switches default to `ON`. For a full development build, also install GoogleTest and standalone Asio:

```bash
sudo apt-get install libgtest-dev libasio-dev
cmake --preset debug
cmake --build --preset debug -j4
ctest --preset debug
```

`CMakePresets.json` provides `debug`, `release`, and matching vcpkg presets. Set `VCPKG_ROOT` and pick `vcpkg-debug` or `vcpkg-release` when using vcpkg; the manifest covers dependencies for both the library and development builds.

## Documentation

Design and source-code walkthroughs (currently in Chinese):

| Document | Contents |
| --- | --- |
| [Coroutine Encapsulation](docs/协程封装.md) | Task frame protocol, task context, this_coro, wait nodes, and the scheduling boundary |
| [Coroutine Concurrency](docs/协程并发.md) | spawn / block_on, join_handle, and the completion boundaries of join / select / scope |
| [Coroutine Synchronization](docs/协程同步.md) | Wait-node protocol, mutex, condition variable, semaphore, latch, barrier, and MPSC |
| [Async Runtime](docs/异步运行时.md) | Runtime components, coroutine scheduling, I/O driving, startup and shutdown |
| [Async I/O](docs/异步IO.md) | epoll/kqueue, file services, buffers and algorithms, cancellation and lifecycle |
| [Network I/O](docs/网络IO.md) | Mixin and CRTP design of the TCP / UDP interfaces with key source walkthroughs |
| [Timer](docs/定时器.md) | Multi-level timing wheel, sleep, periodic ticks, and I/O timeouts |

Benchmark methodology and reproduction steps are documented in the [benchmark README](benchmark/README.md).

## License

[Apache-2.0](LICENSE)
