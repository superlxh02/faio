# faio · Fast Async IO

[简体中文](README.md) | **English**

faio is a Linux asynchronous I/O library written in modern C++: it adopts C++20 coroutines as the execution model and Linux io_uring as the native Proactor async engine, providing coroutine tasks, a multi-threaded runtime, network and file I/O, timers, and coroutine synchronization primitives. The library is header-only and licensed under [Apache-2.0](LICENSE).

With faio, asynchronous flows read like synchronous code: when awaiting an I/O completion, a timer expiration, or a synchronization primitive, the current coroutine suspends and yields its worker thread; once the completion arrives, the runtime reschedules and resumes it. No thread is ever blocked on a read, a write, or a wait, which makes the library suitable for high-concurrency network services, asynchronous task processing, and general coroutine-based concurrent programs.

[Features](#features) · [Highlights](#highlights) · [Examples](#examples) · [Requirements](#requirements) · [Integration](#integration) · [Documentation](#documentation)

## Features

| Module | Capabilities |
| --- | --- |
| Coroutine tasks | Lazy `task<T>`, task launching and result handles, exception propagation, cooperative cancellation |
| Concurrency combinators | `join`, `join_all`, `select`, structured task scope `scope` |
| Async runtime | Default runtime, standalone runtimes, multi-worker scheduling, work stealing |
| Network I/O | TCP listeners and connections, stream read/write, UDP datagrams, IPv4 / IPv6 addresses |
| Low-level I/O | io_uring-based awaitables for file and socket operations |
| Time operations | Sleep, deadlines, I/O timeouts, periodic timers |
| Synchronization primitives | Mutex, semaphore, condition variable, latch, barrier, bounded MPSC queue |
| Logging | spdlog with runtime level adjustment and sink configuration |

## Highlights

- **C++20 stackless coroutines**: the task model is built on C++20 stackless coroutines. `task<T>` is a lazy, move-only coroutine task; nested `co_await` continues into the child through symmetric transfer, with no extra stack allocation or scheduling overhead. Results and exceptions propagate along the await chain, so asynchronous errors are handled with standard `try/catch`.
- **Proactor I/O model**: io_uring handles asynchronous submission and completion notification; the runtime turns completions into ready coroutines.
- **Worker-thread model**: the runtime consists of a fixed set of worker threads, each running its own event loop that, within a single loop, executes ready coroutines, reaps io_uring completions, processes timer expirations, and handles cross-thread wakeups. A suspended coroutine yields its thread, so no task can block the worker it runs on.
- **Work-stealing scheduling**: a two-level structure of per-worker local ready queues plus a shared global queue. Same-thread submissions go straight to a fast slot in the local queue, cross-thread submissions go to the global queue, and idle workers steal batches of tasks from other workers' local queues, reducing lock contention while keeping the load balanced across cores.
- **Coroutine-semantics synchronization primitives**: mutex, semaphore, condition variable, latch, barrier, and a bounded MPSC queue are all coroutine-level primitives—contention suspends the current coroutine instead of blocking the thread, and resumption is delivered back to the original scheduling domain. Every wait supports cooperative cancellation via stop tokens.
- **Coroutinized async network and file I/O**: TCP listen/connect, stream read/write, UDP datagrams, and low-level file and socket operations are uniformly exposed as `co_await`-able awaitables. A network service can be written in a synchronous style with one coroutine per connection, gaining timeout and cooperative cancellation support naturally.

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

The default runtime starts on first use; call `faio::runtime::configure()` beforehand to adjust worker count and other parameters. See [examples](examples) for complete programs, including [coroutines and concurrency](examples/coroutine_task.cpp), [synchronization primitives](examples/sync_demo.cpp), a [TCP server](examples/tcp_server.cpp), and a [UDP server](examples/udp_server.cpp).

Logs are written to stderr at `info` level by default, with timestamps, levels, and thread IDs. See [runtime logging](docs/异步运行时.md#6-诊断日志) for configuration (documentation is currently available in Chinese).

## Requirements

| Item | Requirement |
| --- | --- |
| Operating system | Linux with kernel support for the io_uring operations in use; containers depend on the host kernel |
| Compiler and standard library | C++23 support including coroutines, `std::expected`, and `std::format`; verified with GCC 15 |
| Build tools | CMake 3.20+, pkg-config; presets use Ninja |
| Library dependencies | liburing, spdlog, threads |
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

The target automatically propagates the C++23 requirement, include paths, and the liburing / spdlog / threads link dependencies. Include `<faio/faio.hpp>` in your application to use the full library.

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

Pass `-DCMAKE_PREFIX_PATH=/path/to/faio-install` when configuring the application, and make sure liburing, spdlog, and pkg-config are available.

### Building examples and tests

On a Linux system with a C++23 compiler, install dependencies from system packages. The following commands use an Ubuntu development environment:

```bash
sudo apt-get install g++ cmake ninja-build pkg-config liburing-dev libspdlog-dev

git clone https://github.com/superlxh02/faio.git
cd faio
cmake --preset release -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset release -j4
./build/examples/coroutine_task
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
| [Async I/O](docs/异步IO.md) | io_uring wrappers and I/O awaitables: submission, completion, and timeouts |
| [Network I/O](docs/网络IO.md) | Mixin and CRTP design of the TCP / UDP interfaces with key source walkthroughs |
| [Timer](docs/定时器.md) | Multi-level timing wheel, sleep, periodic ticks, and I/O timeouts |

Benchmark methodology and reproduction steps are documented in the [benchmark README](benchmark/README.md).

## License

[Apache-2.0](LICENSE)
