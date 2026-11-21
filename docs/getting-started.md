# 快速开始

这一页带你在本机编译并运行第一个 faio 程序：示例是 `examples/quickstart.cpp`，运行后输出 `The answer is 42`。文中所有命令都从仓库根目录执行。

## 工具链与依赖

主线使用 C++23、CMake 3.20+ 和 Ninja。编译器需要支持协程、`std::expected`、`std::format` 和显式对象参数，CMake 会在配置阶段编译探测代码，逐一验证这些能力。

| 平台 | 主线工具链 | 依赖准备 |
| --- | --- | --- |
| Linux | Clang 23，同时安装 GCC | 用包管理器安装 spdlog、OpenSSL 和 liburing；完整开发构建另需 GoogleTest 和 standalone Asio，可选 gperftools |
| macOS | Homebrew LLVM Clang 23，同时安装 GCC | `brew install llvm gcc cmake ninja spdlog openssl googletest asio`，并将 LLVM 的 bin 目录加入 PATH |
| Windows | MSVC、clang-cl 或 MinGW-w64 | `scripts/bootstrap_windows.ps1` 准备固定版本的依赖，`scripts/windows_environment.ps1` 选择当前工具链 |

不想在本机装依赖的话，可以按 [Docker 构建说明](../docker/README.md)使用现成的 Linux 镜像。依赖全部通过系统包全局安装，镜像里同时提供 vcpkg 供自定义工程使用；CMake 配置阶段不会联网下载依赖。

## 获取源码

```sh
git clone https://github.com/superlxh02/faio.git
cd faio
```

## 主线构建

Linux 上第一次构建可以关掉测试和基准，只编示例：

```sh
cmake --preset linux-main-release \
  -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset linux-main-release --target quickstart
./build/linux-main-release/examples/quickstart
```

macOS 使用 Homebrew 安装的 LLVM 工具链：

```sh
export PATH="$(brew --prefix llvm)/bin:$PATH"
cmake --preset mac-main-release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix);$(brew --prefix openssl)" \
  -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset mac-main-release --target quickstart
./build/mac-main-release/examples/quickstart
```

Windows 上先运行依赖脚本，再选择一种工具链，这里以 MSVC 为例：

```powershell
./scripts/bootstrap_windows.ps1
. ./scripts/windows_environment.ps1 msvc
cmake --preset windows-msvc-main-release -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset windows-msvc-main-release --target quickstart
./build/windows-msvc-main-release/examples/quickstart.exe
```

clang-cl 和 MinGW 分别使用 `windows-clang-main-release`、`windows-mingw-main-release` 预设，环境脚本参数对应为 `clang`、`mingw`。三种工具链各有独立的构建目录，避免混用 ABI。

## 完整程序

示例通过 `<faio/faio.hpp>` 拿到全部公开接口。其他常用入口见 [API 介绍](api/index.md#包含哪个头文件)。

```cpp
{{#include ../examples/quickstart.cpp}}
```

这段代码里，`task<int>` 定义一个惰性协程，`spawn()` 把它提交给运行时，`join()` 让两个分支并发汇合。协程内部用 `co_await` 等待异步结果，`main` 所在的普通线程则用 `block_on()` 阻塞到整个任务完成。

## 构建配置与测试

CMake 的 configure、build、test 三组预设同名，`release` 与 `debug` 分别对应 Release 和 Debug 构建：

| 平台 | 主线预设 | 全特性预设 |
| --- | --- | --- |
| Linux | `linux-main-release/debug` | `linux-all-release/debug` |
| macOS | `mac-main-release/debug` | `mac-all-release/debug` |
| Windows clang-cl | `windows-clang-main-release/debug` | `windows-clang-all-release/debug` |
| Windows MinGW | `windows-mingw-main-release/debug` | `windows-mingw-all-release/debug` |
| Windows MSVC | `windows-msvc-main-release/debug` | `windows-msvc-all-release/debug` |

表中写成 `release/debug` 的条目代表两个独立预设，例如 `linux-main-release` 和 `linux-main-debug`。主线预设启用稳定特性，全特性预设额外打开实验层。实验层当前要求 **Linux + GCC 16.1.x + libstdc++ 16 + C++26**；macOS 和 Windows 的 all 预设在配置时会报告这一工具链约束，这两个平台请使用 main 预设。

Linux 上做全特性构建之前，先运行 `scripts/bootstrap_experimental_deps.py` 准备锁定版本的 stdexec，然后切到 GCC 16.1 工具链：

```sh
python3 scripts/bootstrap_experimental_deps.py
cmake --preset linux-all-release
cmake --build --preset linux-all-release -j4
ctest --preset linux-all-release
```

主线的单元测试这样跑：

```sh
cmake --preset linux-main-debug
cmake --build --preset linux-main-debug -j4
ctest --preset linux-main-debug
```

Linux 在编译期选择后端：`FAIO_ENABLE_IO_URING=OFF` 使用 epoll，`ON` 使用 io_uring（需要 liburing）。macOS 固定使用 kqueue，Windows 固定使用 IOCP。

`FAIO_ALLOCATOR=AUTO` 表示自动选择：Linux 上检测到 gperftools 且未启用检测器时使用 tcmalloc，否则回退到系统分配器。设为 `SYSTEM` 固定使用系统分配器，设为 `TCMALLOC` 则强制要求 tcmalloc 可用。tcmalloc 会替换整个进程的 malloc/new，协程帧缓存与大帧池不受影响。如果你自行给目标添加了检测器参数，记得同时把分配器设为 `SYSTEM` 或声明 `FAIO_SANITIZERS`；配置细节见 [Linux 分配器](../docker/README.md#linux分配器)。

## 集成到自己的工程

把 faio 作为子目录引入（源码集成）：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_async_app LANGUAGES CXX)

set(FAIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/faio)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE faio::faio)
```

如果使用的是安装导出的 faio，消费工程改用 `find_package(faio CONFIG REQUIRED)`，链接目标同样是 `faio::faio`。TLS 与进程组件分别通过 `FAIO_ENABLE_TLS`、`FAIO_ENABLE_PROCESS` 开关启用，用法见 [TLS](api/tls.md)与[异步进程](api/process.md)。

[认识 faio](learn-faio.md) · [API 介绍](api/index.md) · [API 查询](api-reference.md)
