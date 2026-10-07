# faio · Fast Async IO

[简体中文](README.md) | **English**

faio is a high-performance, cross-platform asynchronous I/O library built on C++20 coroutines (requiring a C++23 toolchain). It runs on Linux (dual io_uring/epoll backends), macOS (kqueue), and Windows (native IOCP/OVERLAPPED), and provides coroutine tasks and synchronization primitives, single/multi-thread runtimes, TCP/UDP networking, Unix-specific network extensions, timers, and native asynchronous file I/O (operations without native support fall back to an isolated bounded service). The library is header-only and licensed under [Apache-2.0](LICENSE).

With faio, asynchronous flows read like synchronous code: a coroutine suspends and yields its worker thread while awaiting I/O, a timer, or a synchronization primitive, and the runtime resumes it on completion. io_uring-capable file operations are submitted as native SQEs and the rest run on an isolated bounded service, so workers never block on I/O—suited to high-concurrency network services, asynchronous task processing, and general coroutine-based concurrent programs.

[Features](#features) · [Highlights](#highlights) · [Examples](#examples) · [Requirements](#requirements) · [Third-party dependencies](#third-party-dependencies) · [Building from source](#building-from-source) · [Integration](#integration) · [Documentation](#documentation)

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
| Experimental features | C++26 reflection-annotated async entry with compile-time runtime configuration, and stdexec Sender/Receiver interoperability (Linux + GCC 16.1 only, behind a separate build switch; the base library is unaffected) |

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

### Experimental: annotated entry and Sender interop

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
    // exec::task does not propagate faio queries; carry the runtime explicitly
    co_await stdexec::schedule(runtime.get_multi_thread_scheduler());
    co_return co_await runtime.as_sender(native_job());
}

// Annotations declare the entry and runtime configuration;
// the library generates the real main, no hand-written block_on needed
[[=faio::experimental::main(async_main)]]
[[=faio::experimental::runtime_options{
    .mode = faio::runtime::mode::multi_thread,
    .workers = 4,
    .io_backend = faio::runtime::io_backend::IO_EPOLL}]]
faio::task<int> async_main(int, char**) {
    auto runtime = faio::experimental::this_runtime();
    auto result = runtime.spawn_sender(workflow(runtime)); // submit the sender graph, get an awaitable handle
    const auto values = co_await std::move(result);
    co_return values && std::get<0>(*values) == 42 ? 0 : 1;
}
```

The experimental features require Linux + GCC 16.1 + C++26, built with `FAIO_ENABLE_EXPERIMENTAL` and the `linux-gcc16.1-experimental-*` presets; see [Experimental Features](docs/实验性特性.md) (Chinese) for the architecture and source walkthrough, and [examples/experimental](examples/experimental/) for complete programs.

The default runtime starts on first use; call `faio::runtime::configure()` beforehand to adjust worker count and other parameters. The examples cover [coroutine basics and concurrency](examples/coroutine_basics.cpp), [coroutine synchronization](examples/coroutine_sync.cpp), [TCP echo](examples/tcp_echo_server.cpp), [UDP echo](examples/udp_echo_server.cpp), a [shared TCP counter application](examples/tcp_counter_server.cpp), [blocking work alongside async tasks](examples/blocking_thread_pool.cpp), [TCP echo with current-thread scheduling](examples/tcp_echo_server_single_thread.cpp), and [file and directory operations](examples/file_and_directory.cpp). See the [example guide](examples/README.md) for detailed Chinese explanations and commands; `tcp_counter_server --self-test` runs a finite multi-client scenario.

The default is `multi_thread`; calling `set_mode()` is optional. To drive coroutines on the thread calling `block_on`, configure `faio::config_builder{}.set_mode(faio::runtime::mode::current_thread).build()` before first use; see the [current-thread TCP example](examples/tcp_echo_server_single_thread.cpp). This mode starts no background async worker. Queues, timers and in-flight I/O persist between calls, and submitted async tasks advance while `block_on` runs. `block_on` waits for its task group, without waiting for independently submitted background roots. `spawn_blocking` still uses a separate blocking pool; see the [blocking task example](examples/blocking_thread_pool.cpp). The differences between current-thread and multi-thread scheduling are covered in [Async Runtime](docs/异步运行时.md) (Chinese).

Logs are written to stderr at `info` level by default, with timestamps, levels, and thread IDs. See [runtime logging](docs/异步运行时.md#7-诊断日志) for configuration (documentation is currently available in Chinese).

## Requirements

| Item | Requirement |
| --- | --- |
| C++ standard | **C++23**: coroutines, `std::expected`, and `std::format`; the `faio::faio` target propagates `cxx_std_23` and CMake compiles a probe for all three at configure time |
| Compiler — macOS | Homebrew LLVM **Clang ≥ 22** (`brew install llvm`; currently validated with Clang 23 at `/opt/homebrew/opt/llvm`); the system AppleClang standard library is insufficient and rejected at configure time |
| Compiler — Windows | **MSVC** (validated with MSVC 19.51 from Visual Studio 18), **clang-cl** (validated with LLVM Clang 22.1.0, using the Microsoft STL and MSVC ABI), **MinGW-w64** (validated with MSYS2 UCRT64 GCC 16.2.0-4; native TLS is mandatory, emutls toolchains are rejected at configure time) |
| Compiler — Linux | **Clang ≥ 22** (validated with Ubuntu Clang 22.1.2 plus libstdc++ 15) or **GCC**; GCC must include the [coroutine constructor exception cleanup fix](https://github.com/gcc-mirror/gcc/commit/5422486d4bc728688dd2c874cb014fadf745b2da)—GCC 15.2.0-16ubuntu1 measurably leaks the coroutine frame when a frame parameter copy or move throws and is not a fully exception-safe toolchain |
| Build tools | CMake 3.20+; presets use Ninja |
| Third-party libraries | spdlog (required public link dependency) and threads; liburing (only for the Linux io_uring backend; epoll-only builds do not need it) |
| Optional dev dependencies | GoogleTest for unit tests; standalone Asio for TCP benchmark comparison |
| Optional benchmark tools | Rust / Cargo, wrk, Python 3 with numpy / pandas / matplotlib — see [benchmark](benchmark/README.md) |

Platform I/O backends: Linux uses dual io_uring/epoll backends (both are built by default, with io_uring preferred on kernel 5.10+), macOS uses kqueue, and Windows uses native IOCP/OVERLAPPED (targeting Windows 10/11 x64 with `_WIN32_WINNT=0x0A00`).

## Third-party dependencies

faio is header-only; third-party dependencies are needed only at build time, and **CMake never downloads anything during configure**. Provide them in one of the following ways:

| Approach | When to use | How |
| --- | --- | --- |
| **vcpkg manifest** | Cross-platform, centrally managed dependencies | The repository ships a `vcpkg.json` manifest (spdlog, gtest, asio, liburing[linux]). Run `vcpkg install`, then configure with `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` |
| **System / installed packages** | Default on Linux and macOS | Dependencies installed via apt / brew or built manually are located with `find_package(... CONFIG)`; pass `-DCMAKE_PREFIX_PATH=...` for non-standard prefixes. Corresponds to `FAIO_USE_WORKSPACE_DEPS=OFF` (the default off Windows) |
| **Pinned source builds** | Default on Windows | `scripts/bootstrap_windows.ps1` downloads and verifies spdlog 1.15.3, GoogleTest 1.17.0, and Asio 1.36.0 with the versions, archive URLs, and SHA256 hashes pinned in `scripts/windows-dependencies.json`, extracting them into a sibling `faio-deps/` directory; CMake compiles them with the current toolchain via `add_subdirectory` (`FAIO_USE_WORKSPACE_DEPS=ON`, the Windows default), so each build directory uses its own compiler and MSVC / MinGW ABIs are never mixed |

Notes:

- liburing is located by `cmake/FindLiburing.cmake`, which also compiles and links a probe for the required APIs and exports the `Liburing::Liburing` imported target.
- spdlog is a public link dependency of `faio::faio` (`target_link_libraries(faio INTERFACE ... spdlog::spdlog)`); consumers linking `faio::faio` need no extra handling.
- GoogleTest and Asio are looked up on demand by the test and benchmark subdirectories only.
- Override the pinned-source cache location with `-DFAIO_DEPENDENCY_ROOT=` (default: the sibling `faio-deps` directory).

## Building from source

The four switches `FAIO_BUILD_EXAMPLES`, `FAIO_BUILD_TESTS`, `FAIO_BUILD_BENCHMARKS`, and `FAIO_INSTALL` all default to `ON`. `CMakePresets.json` provides `windows-msvc`, `windows-clang`, `windows-mingw`, and `windows-clang-asan` on Windows, `linux-clang22-dual` and `linux-clang22-epoll` on Linux, and `macos-clang23` on macOS; configure, build, and test presets share the same name except for the two Linux dual-backend test presets. For custom Debug, Docker, or vcpkg builds, pass options explicitly to `cmake -S . -B <build-directory>`.

### Windows (MSVC / clang-cl / MinGW-w64)

In PowerShell, prepare the pinned dependencies and toolchains first, then build with one of the three compilers:

```powershell
./scripts/bootstrap_windows.ps1           # first run: pinned deps + MSYS2 UCRT64 MinGW toolchain (-Offline validates the cache)
. ./scripts/windows_environment.ps1 msvc  # or clang / mingw
cmake --preset windows-msvc               # or windows-clang / windows-mingw
cmake --build --preset windows-msvc
ctest --preset windows-msvc
./build/windows-msvc/examples/coroutine_basics.exe
```

The full three-compiler matrix (build, test, per-header installed-package compilation, and a two-translation-unit consumer check) runs via `./scripts/build_windows.ps1 -Offline`, with logs under `build/windows-matrix`.

MinGW notes: the toolchain must use native TLS (MSYS2 GCC 16 and later); CMake assembles a probe and rejects emutls. The [multi-translation-unit TLS defect](https://sourceforge.net/p/mingw-w64/bugs/994/) of MinGW GCC is handled by a compile launcher enabled only for Windows GCC, which localizes one internal initialization alias after each object compiles; it requires Python 3 plus MinGW's nm/objcopy, and GCC LTO is not supported (the project disables LTO in Release anyway). MinGW executables need the same-distribution DLLs from `faio-deps/tools/msys2-ucrt64/ucrt64/bin`, which the environment script adds to the current process PATH.

clang-cl AddressSanitizer validation uses a separate preset, instrumenting faio consumers, spdlog, and GoogleTest consistently:

```powershell
. ./scripts/windows_environment.ps1 clang
cmake --preset windows-clang-asan
cmake --build --preset windows-clang-asan
ctest --preset windows-clang-asan
```

### Linux (Clang / GCC)

On a Linux system with Clang 22 and a C++23 standard library installed, install the dependencies from system packages. The following commands use an Ubuntu development environment:

```bash
sudo apt-get install cmake ninja-build liburing-dev libspdlog-dev

git clone https://github.com/superlxh02/faio.git
cd faio
cmake --preset linux-clang22-dual
cmake --build --preset linux-clang22-dual -j4
./build/linux-clang22-dual/examples/coroutine_basics
```

The dual backends share a single build; run the two test presets without recompiling:

```bash
ctest --preset linux-clang22-dual-epoll
ctest --preset linux-clang22-dual-uring
```

Without liburing, use the `linux-clang22-epoll` preset (test preset of the same name). For a full development build (tests and benchmarks), also install GoogleTest and standalone Asio:

```bash
sudo apt-get install libgtest-dev libasio-dev
```

See [Requirements](#requirements) for the GCC toolchain version requirement.

**One-command Docker build**: without preparing a local toolchain, compile and test in one shot with Docker (build context is the repository root):

```bash
docker build -f docker/build.dockerfile -t faio:build .
docker run --rm --security-opt seccomp=unconfined faio:build \
  ctest --test-dir build --output-on-failure
```

`seccomp=unconfined` allows tests and examples to call io_uring; the layered environment image, build arguments (`DEV_IMAGE`, `CMAKE_BUILD_TYPE`, `BUILD_JOBS`), and mount-based development workflow are documented in [docker/README.md](docker/README.md).

### macOS (Homebrew Clang)

```bash
brew install llvm cmake ninja spdlog googletest

cmake --preset macos-clang23
cmake --build --preset macos-clang23 -j4
ctest --preset macos-clang23
./build/macos-clang23/examples/coroutine_basics
```

The preset selects `/opt/homebrew/opt/llvm/bin/clang++` and looks up dependencies under `/opt/homebrew`; to build only the library, disable tests and benchmarks and skip GoogleTest / Asio.

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

Pass `-DCMAKE_PREFIX_PATH=/path/to/faio-install` when configuring the application, and make sure spdlog and threads are available. To build this repository's own examples and tests from source, see [Building from source](#building-from-source) above.

## Documentation

Design and source-code walkthroughs (currently in Chinese):

| Document | Contents |
| --- | --- |
| [Coroutine Encapsulation](docs/协程封装.md) | Task frame protocol, task context, this_coro, wait nodes, and the scheduling boundary |
| [Coroutine Concurrency](docs/协程并发.md) | spawn / block_on, join_handle, and the completion boundaries of join / select / scope |
| [Coroutine Synchronization](docs/协程同步.md) | Wait-node protocol, mutex, condition variable, semaphore, latch, barrier, and MPSC |
| [Async Runtime](docs/异步运行时.md) | Runtime components, coroutine scheduling, I/O driving, startup and shutdown |
| [Async I/O](docs/异步IO.md) | io_uring native completions, epoll/kqueue, the Windows IOCP backend, file services, buffers and algorithms, cancellation and lifecycle |
| [Network I/O](docs/网络IO.md) | Mixin and CRTP design of the TCP / UDP interfaces with key source walkthroughs |
| [Timer](docs/定时器.md) | Multi-level timing wheel, sleep, periodic ticks, and I/O timeouts |
| [Experimental Features](docs/实验性特性.md) | Annotated async entry, compile-time runtime configuration, and the stdexec Sender/Receiver adaptation |

Benchmark methodology and reproduction steps are documented in the [benchmark README](benchmark/README.md).

## License

[Apache-2.0](LICENSE)
