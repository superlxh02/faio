# faio · Fast Async IO

[简体中文](README.md) | **English**

[Website](https://superlxh02.github.io/faio/) · [Quick start](docs/getting-started.md) · [Learn faio](docs/learn-faio.md) · [API guide](docs/api/index.md) · [Doxygen reference](https://superlxh02.github.io/faio/api/)

faio is a header-only asynchronous I/O library for C++23, built on coroutines and licensed under Apache-2.0. Linux backends are epoll and io_uring; macOS uses kqueue and Windows uses IOCP.

| Area | Interface |
| --- | --- |
| Tasks and concurrency | `task`, `spawn`, `join`, `select`, `scope` |
| Runtime | single-thread and multi-thread scheduling, cooperative cancellation, bounded blocking-task service |
| Networking and files | TCP, UDP, Unix sockets, asynchronous file and directory operations |
| Time and synchronization | timers, timeouts, mutexes, semaphores, condition variables, bounded MPSC |
| Optional components | OpenSSL TLS, asynchronous processes |
| Experimental layer | C++26 reflection annotation entry, stdexec Sender/Receiver interoperability |

## Quick start

The mainline toolchain is Clang 23; Windows also supports MSVC and MinGW-w64. The Linux image installs GCC as well, and system packages provide the third-party dependencies. Install CMake 3.20+, Ninja, spdlog and any optional dependencies you need, then run:

```sh
cmake --preset linux-main-release
cmake --build --preset linux-main-release -j4
ctest --preset linux-main-release
./build/linux-main-release/examples/quickstart
```

Use `mac-main-release` on macOS. The Windows presets are `windows-clang-main-release`, `windows-mingw-main-release` and `windows-msvc-main-release`. The `bootstrap_windows.ps1` and `windows_environment.ps1` scripts prepare dependencies and the selected environment; toolchain setup and per-platform commands are in the [quick start](docs/getting-started.md).

```cpp
#include <faio/faio.hpp>
#include <chrono>

faio::task<int> delayed_value(int value) {
    co_await faio::time::sleep(std::chrono::milliseconds{10});
    co_return value;
}

faio::task<int> sum() {
    auto [left, right] = co_await faio::join(delayed_value(20), delayed_value(22));
    co_return left + right;
}

int main() {
    const int result = faio::block_on(sum());
    return result == 42 ? 0 : 1;
}
```

## Build configurations

Configure, build and test use the same preset names; each preset has a `release` and a `debug` variant.

| Platform | Mainline | All features |
| --- | --- | --- |
| Linux | `linux-main-*` | `linux-all-*` |
| macOS | `mac-main-*` | `mac-all-*` |
| Windows clang-cl | `windows-clang-main-*` | `windows-clang-all-*` |
| Windows MinGW | `windows-mingw-main-*` | `windows-mingw-all-*` |
| Windows MSVC | `windows-msvc-main-*` | `windows-msvc-all-*` |

`main` enables stable features and `all` adds the experimental layer. The experimental layer currently requires Linux, GCC 16.1.x, matching libstdc++ 16 and C++26; the macOS and Windows `all` presets report the toolchain constraint at configure time. Prepare pinned experimental dependencies with `scripts/bootstrap_experimental_deps.py`.

`FAIO_ENABLE_IO_URING` selects the Linux backend at compile time. `FAIO_ENABLE_TLS` and `FAIO_ENABLE_PROCESS` control optional components. `FAIO_BUILD_EXAMPLES`, `FAIO_BUILD_TESTS` and `FAIO_BUILD_BENCHMARKS` control development targets; `FAIO_INSTALL` controls the install export.

## Integration

Install spdlog, then add the source project:

```cmake
set(FAIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/faio)
target_link_libraries(my_app PRIVATE faio::faio)
```

Installed consumers use `find_package(faio CONFIG REQUIRED)`. The `faio::faio` target carries C++23, include, thread and spdlog requirements. Optional targets are `faio::tls` and `faio::process`.

## Docker

| File | Purpose |
| --- | --- |
| [docker/dev-image.dockerfile](docker/dev-image.dockerfile) | CLion toolchain environment, system dependencies and a fallback vcpkg |
| [docker/duild-all.dockerfile](docker/duild-all.dockerfile) | GCC 16.1, all-feature build |
| [docker/build-main.dockerfile](docker/build-main.dockerfile) | Clang 23, stable-feature build, with GCC also installed |
| [Dockerfile](Dockerfile) | Default all-feature build |

Commands and image configuration are in the [Docker guide](docker/README.md).

## Documentation

- [Quick start](docs/getting-started.md): dependencies, platform toolchains, build and integration.
- [Learn faio](docs/learn-faio.md): a ten-chapter tutorial built around one TCP echo service, plus topic guides and a glossary.
- [API guide](docs/api/index.md): public headers, calling conventions and lifetimes.
- [API reference](docs/api-reference.md): types, functions and overloads.
- [Documentation tools](.github/site/README.md): local build and site maintenance.

Complete programs are in [examples](examples/README.md); performance benchmarks are in [benchmark](benchmark/README.md).

The project documentation and API descriptions are in Chinese.

[Apache-2.0](LICENSE)
