# faio 模块支持

用 `import faio;` 使用稳定 API，也可以按功能单独导入模块。接口源码位于 `cxx-module/`，均为 `.cppm`，与头文件版共用 `include/faio/` 的实现，使用 C++23 编译。

同一项目、同一构建配置只接入一个模块目标，所有消费者复用编译结果。一个最终程序选择头文件或模块接入中的一种。

## 接入项目

需要 spdlog、CMake 3.28+、Ninja 1.11+ 和支持 C++23 模块的 Clang。`clang++` 与 `clang-scan-deps` 必须来自同一份 LLVM 安装；macOS 使用 Homebrew LLVM，并将其 `bin` 加入 `PATH`。

将完整 faio 仓库放在应用的 `third_party/faio/`，在应用根目录创建 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.28)
project(my_app LANGUAGES CXX)

add_subdirectory(third_party/faio/cxx-module faio-modules)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE faio::module)
set_target_properties(my_app PROPERTIES CXX_SCAN_FOR_MODULES ON CXX_EXTENSIONS OFF)
```

`main.cpp` 可直接采用 [quickstart.cpp](../examples/module/quickstart.cpp)，它通过 `import faio;` 使用协程，成功时返回 0。在应用根目录构建并运行：

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DFAIO_ENABLE_IO_URING=OFF
cmake --build build
./build/my_app
```

示例关闭 io_uring，便于在受限 Linux 容器中运行；Linux 默认开启它，macOS 使用 kqueue。

使用安装包时，将 `add_subdirectory(...)` 换成 `find_package(faioModules CONFIG REQUIRED)`，链接目标仍为 `faio::module`。配置时用 `CMAKE_PREFIX_PATH` 指向安装前缀，完整命令见[安装与消费示例](../examples/module/README.md#从安装包接入)。消费方会重建 BMI，编译器、标准库和构建配置须与提供方兼容。

## 按功能导入

`import faio;` 聚合以下稳定模块，也可以只导入需要的模块：

| 导入 | 提供的接口 |
| --- | --- |
| `import faio.common;` | 错误类型、`expected`，对应 [error.hpp](../include/faio/common/error.hpp) |
| `import faio.coroutine;` | `task`、`join`、`this_coro` 等协程 API |
| `import faio.log;` | 日志 |
| `import faio.io;` | I/O 操作、内存流与双工流 |
| `import faio.stdio;` | 协程标准流与格式化输出 |
| `import faio.fs;` | 文件与目录 |
| `import faio.net;` | 网络 |
| `import faio.runtime;` | 运行时与 `spawn_blocking` |
| `import faio.sync;` | 同步原语与通道 |
| `import faio.time;` | 定时器与时间操作 |

`util` 和 `blocking` 是内部实现，不提供公共导入入口。导入 `faio.coroutine` 或依赖它的模块时，无需额外包含 `<coroutine>`。

## 可选功能

TLS 与异步进程默认关闭，构建时按需开启：

| 功能 | CMake 配置 | 单独导入 |
| --- | --- | --- |
| TLS | `-DFAIO_ENABLE_TLS=ON`，需要 OpenSSL 3.5+ | `import faio.tls;` |
| 异步进程 | `-DFAIO_ENABLE_PROCESS=ON` | `import faio.process;` |

启用后继续链接 `faio::module`，`import faio;` 会包含对应接口。安装消费可用 `COMPONENTS tls` 或 `COMPONENTS process` 要求包中必须提供该功能；独立的 `faio::module_tls`、`faio::module_process` 目标保留为兼容入口。

## Bazel 与 xmake

两种工具均可从源码接入，目标和配置见下表；在仓库中运行示例的命令见[模块示例](../examples/module/README.md#用-bazel-或-xmake-运行)。

| 工具 | 核心目标 | 下游配置 |
| --- | --- | --- |
| Bazel | `//cxx-module:faio`，外部依赖使用 `@faio//cxx-module:faio` | [Bzlmod 与模块扫描](../docs/getting-started.md#bazel-下游项目) |
| xmake | `faio_module` | [源码与安装包接入](../docs/getting-started.md#xmake-下游项目) |

## 实验接口

实验 API 通过 `import faio.experimental;` 单独导入，需要 Linux、GCC 16.1.x、配套 libstdc++ 16 和 C++26。CMake 构建还需要 4.1+，配置 `-DFAIO_MODULE_ENABLE_EXPERIMENTAL=ON` 并链接 `faio::module_experimental`。该入口受下述 GCC 限制影响。

反射代码先包含 `<meta>`；异步入口宏在导入后包含 `<faio-module/entry.hpp>`。execution 默认关闭，锁定的 stdexec 在 GCC 16.1 的模块构建中会触发编译器内部错误；Sender/Receiver 桥接使用头文件版。API 见[实验性接口](../docs/api/experimental.md)，依赖准备见[实验依赖](../scripts/README.md#实验依赖)。

## 已知限制

GCC 16.1.1 构建完整核心模块时，会在 `faio.runtime` 读取 `faio.coroutine` 的 BMI 时报 `Bad file data`；`faio.process` 也可能在读取核心模块时遇到同类错误。完整模块接入使用 Clang 验证。

AppleClang 无法通过当前 CMake 配置扫描模块，macOS 使用 Homebrew LLVM。平台工具链和其他构建命令见[构建指南](../docs/getting-started.md#faio-模块支持)。
