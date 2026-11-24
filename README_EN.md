# faio · Fast Async IO

[简体中文](README.md) | **English**

[Website](https://superlxh02.github.io/faio/) · [Quick start](docs/getting-started.md) · [Learn faio](docs/learn-faio.md) · [API guide](docs/api/index.md) · [Doxygen reference](https://superlxh02.github.io/faio/api/)

faio is a C++23 asynchronous I/O library, header-only by default. Networking, files, timers and synchronization primitives are exposed as coroutine interfaces you `co_await`, while the runtime handles scheduling and I/O. The Linux backend is epoll or io_uring, macOS uses kqueue, and Windows uses IOCP; the backend is chosen at compile time, so code above it needs no platform branches. The license is Apache-2.0.

## Quick start

See [Quick start](docs/getting-started.md) for toolchains and dependency installation. The shortest Linux path that only builds `quickstart` is:

```sh
sudo apt install -y libspdlog-dev
cmake --preset linux-main-release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DFAIO_BUILD_MODULES=OFF -DFAIO_ENABLE_TLS=OFF -DFAIO_ENABLE_PROCESS=OFF \
  -DFAIO_ENABLE_IO_URING=OFF -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset linux-main-release --target quickstart
./target/cmake-build-linux-main-release/examples/quickstart
```

Every build writes under `target/`: CMake to `target/cmake-build-<preset>/`, Bazel to `target/bazel/`, xmake to `target/xmake/`, and the Rust comparison benchmarks to `target/cargo/`.

See the [quick start](docs/getting-started.md) for the macOS and Windows flows, including vcpkg dependencies, toolchains and preset selection.

## Minimal example

The program below starts two timed tasks, waits for both, and adds the results. `block_on` drives the runtime on the calling thread until the coroutine returns:

```cpp
#include <chrono>
#include <faio/faio.hpp>

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

With the module build, replace `#include <faio/faio.hpp>` with `import faio;` and leave the rest unchanged. Both entry points include TLS and process interfaces when enabled at build time. Coroutine syntax needs no additional `<coroutine>` include. See the [module integration guide](cxx-module/README.md) for individual modules.

## Bazel and xmake

Both build systems support the header-only library and the C++20 named modules, which share the same implementation in `include/faio/`. Bazel is pinned to 9.2.0; point `CC` at a Clang distribution that supports named modules:

```sh
export CC="$(command -v clang)"
bazel test --config=faio_headers //...
bazel test --config=faio_modules --config=faio_tls --config=faio_process //...
```

`quickstart_module` only imports `faio`, so it uses the same Clang toolchain:

```sh
bazel run --config=faio_modules //examples:quickstart_module
```

xmake needs 3.0.1 or newer. The commands below select Homebrew LLVM on macOS; in the Linux development image use `/usr/sbin/clang++` instead:

```sh
faio_cxx="$(brew --prefix llvm)/bin/clang++"
xmake f -c -y -m release --toolchain=clang \
  --cxx="$faio_cxx" --ld="$faio_cxx" --faio_modules=y
xmake test -j 4
```

Run the same module example after configuring:

```sh
xmake run quickstart_module
```

For a header-only build, use `--config=faio_headers` in Bazel, or drop `--faio_modules=y` and run `xmake build quickstart_header` in xmake. Target names, downstream setup and known limits are in the [module integration guide](cxx-module/README.md) and the [build and test commands](docs/getting-started.md#c20-模块集成和使用).

## Capabilities

Public interfaces grouped by purpose:

| Area | Interface |
| --- | --- |
| Tasks and concurrency | `task`, `spawn`, `join`, `select`, `scope` |
| Runtime | single-thread and multi-thread scheduling, cooperative cancellation, bounded blocking-task service |
| Networking and files | TCP, UDP, Unix sockets, asynchronous file and directory operations |
| Time and synchronization | timers, timeouts, mutexes, semaphores, condition variables, bounded MPSC |
| Optional components | OpenSSL TLS, asynchronous processes |
| Experimental layer | C++26 reflection annotation entry, stdexec Sender/Receiver interoperability |

## Build configurations

CMake uses the same preset name for configure, build and test, and every preset has a `release` and a `debug` variant.

| Platform | Mainline | All features |
| --- | --- | --- |
| Linux | `linux-main-*` | `linux-all-*` |
| macOS | `mac-main-*` | `mac-all-*` |
| Windows clang-cl | `windows-clang-main-*` | `windows-clang-all-*` |
| Windows MinGW | `windows-mingw-main-*` | `windows-mingw-all-*` |
| Windows MSVC | `windows-msvc-main-*` | `windows-msvc-all-*` |

`main` enables stable features and `all` adds the experimental layer. The experimental layer requires Linux, GCC 16.1.x, matching libstdc++ 16 and C++26; the macOS and Windows `all` presets report the toolchain constraint at configure time. `scripts/bootstrap_experimental_deps.py` prepares the pinned experimental dependencies.

`FAIO_ENABLE_IO_URING` selects the Linux backend at compile time. `FAIO_ENABLE_TLS` and `FAIO_ENABLE_PROCESS` control the optional components. `FAIO_BUILD_HEADERS` and `FAIO_BUILD_MODULES` select the build form, and the module form needs CMake 3.28+; `FAIO_BUILD_EXAMPLES`, `FAIO_BUILD_TESTS` and `FAIO_BUILD_BENCHMARKS` control development targets, while `FAIO_INSTALL` controls the install export.

## Integration

The header-only route takes two steps: install spdlog, then point the source dependency at faio:

```cmake
set(FAIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/faio)
target_link_libraries(my_app PRIVATE faio::faio)
```

An installed consumer uses `find_package(faio CONFIG REQUIRED)` instead. The `faio::faio` target carries C++23, include paths, the thread library and spdlog, plus dependencies for TLS and processes enabled at build time. Keep linking `faio::faio` and including `<faio/faio.hpp>` when enabling either component.

For the module form, build and install the module package, then use `find_package(faioModules CONFIG REQUIRED)` and link `faio::module`. `import faio;` exports enabled TLS and process interfaces. Individual imports include `faio.coroutine`, `faio.io` and `faio.net`. Bazel consumers depend on `//cxx-module:faio`, and xmake consumers use `add_deps("faio_module")`. The module requires a matching compiler, standard library and build configuration; details are in the [module integration guide](cxx-module/README.md).

## Docker

The repository defines four images:

| File | Purpose |
| --- | --- |
| [docker/dev-image.dockerfile](docker/dev-image.dockerfile) | CLion toolchain environment, system dependencies and a fallback vcpkg |
| [docker/duild-all.dockerfile](docker/duild-all.dockerfile) | GCC 16.1, all-feature build |
| [docker/build-main.dockerfile](docker/build-main.dockerfile) | Clang 23, stable-feature build, with GCC also installed |
| [Dockerfile](Dockerfile) | Default all-feature build |

The development image ships Bazel 9.2.0 and xmake 3.0.9, so all three build systems work inside the container. Commands and image configuration are in the [Docker guide](docker/README.md).

## Documentation

- [Quick start](docs/getting-started.md): dependencies, platform toolchains, build, module entry points and integration.
- [Learn faio](docs/learn-faio.md): an introduction built around the quickstart and tcp_echo examples, plus topic guides and a glossary.
- [API guide](docs/api/index.md): public headers, calling conventions and lifetimes.
- [API reference](docs/api-reference.md): types, functions and overloads.
- [Documentation tools](.github/site/README.md): local build and site maintenance.

Complete programs are in [examples](examples/README.md); performance benchmarks are in [benchmark](benchmark/README.md).

The project documentation and API descriptions are in Chinese.

[Apache-2.0](LICENSE)
