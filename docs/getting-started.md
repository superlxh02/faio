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

本节的主线构建走 CMake。需要用 C++20 具名模块或 Bazel、xmake 时，工具链要求更高：CMake 3.28+、Ninja 1.11+ 和 `clang-scan-deps`，Bazel 与 xmake 的版本及命令见[具名模块与其他构建入口](#具名模块与其他构建入口)。

## 获取源码

```sh
git clone https://github.com/superlxh02/faio.git
cd faio
```

## 主线构建

这一节用 CMake 预设。Bazel 和 xmake 的构建命令在[具名模块与其他构建入口](#具名模块与其他构建入口)。

Linux 上第一次构建可以关掉测试和基准，只编示例：

```sh
cmake --preset linux-main-release \
  -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset linux-main-release --target quickstart
./target/cmake-build-linux-main-release/examples/quickstart
```

macOS 使用 Homebrew 安装的 LLVM 工具链：

```sh
export PATH="$(brew --prefix llvm)/bin:$PATH"
cmake --preset mac-main-release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix);$(brew --prefix openssl)" \
  -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset mac-main-release --target quickstart
./target/cmake-build-mac-main-release/examples/quickstart
```

Windows 上先运行依赖脚本，再选择一种工具链，这里以 MSVC 为例：

```powershell
./scripts/bootstrap_windows.ps1
. ./scripts/windows_environment.ps1 msvc
cmake --preset windows-msvc-main-release -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset windows-msvc-main-release --target quickstart
./target/cmake-build-windows-msvc-main-release/examples/quickstart.exe
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

## 具名模块与其他构建入口

`import faio;` 提供稳定的任务、组合器、运行时、同步、定时器、流、文件系统、网络、日志及错误接口。TLS 和进程分别通过 `import faio.tls;`、`import faio.process;` 引入。每个最终程序选择一种 faio 消费方式。

将具名模块集成到自己的项目时，可直接使用 [模块移植说明](../cxx-module/README.md) 中的 CMake、Bazel 和 xmake 配置。

`include/faio/` 是两种形态共用的实现。`cxx-module/` 中的入口包含相应公开头，并显式导出已有声明。应用使用 `import` 时复用编译模块接口（BMI）。修改共享头文件会按依赖重新编译相应模块。模块安装包随附共享头和模块接口源码，供下游工具链重建 BMI。

内存双工流的读写协程共用 `include/faio/detail/io/stream/duplex.inc`。头文件版内联这两个函数，模块版将它们编入核心归档。每个最终程序选择一种消费形态。

宏走头文件入口。实验异步入口通过 `faio-module/entry.hpp` 使用共享头中定义的 `main` 宏。

项目的 CMake 预设将整个构建树写入 `target/cmake-build-<preset>/`。Bazel 的输出和便捷链接位于 `target/bazel/`。xmake 默认写入 `target/xmake/`，其中按平台、架构和构建模式放置目标。Rust 基准共用 `target/cargo/`。

### CMake

在仓库根目录使用 CMake 3.28+、Ninja 1.11+ 和同一 LLVM 发行版的编译器及 `clang-scan-deps`。macOS 可用 `export PATH="$(brew --prefix llvm)/bin:$PATH"` 选中 Homebrew LLVM。在项目的 Linux Docker 环境中将下方 `clang++` 换成 `clang++-23`。默认 seccomp 配置下，增加 `-DFAIO_ENABLE_IO_URING=OFF` 选用 epoll。

```sh
cmake -S . -B target/cmake-build-modules -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DFAIO_BUILD_HEADERS=OFF -DFAIO_BUILD_MODULES=ON \
  -DFAIO_ENABLE_TLS=ON -DFAIO_ENABLE_PROCESS=ON
cmake --build target/cmake-build-modules
ctest --test-dir target/cmake-build-modules --output-on-failure
cmake --install target/cmake-build-modules --prefix "$PWD/target/cmake-install-modules"
```

模块可用 `cmake -S cxx-module` 单独配置，源码目录保留同级的 `include/`。安装后的核心消费者用 `find_package(faioModules CONFIG REQUIRED)` 并链接 `faio::module`。使用 TLS 或进程时，在 `find_package` 中加入所需的 `COMPONENTS tls process`，并链接 `faio::module_tls` 或 `faio::module_process`。为导入源启用 `CXX_SCAN_FOR_MODULES`。下游使用匹配的编译器、标准库和构建配置，从安装的接口源码重建 BMI。

```cmake
find_package(faioModules CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE faio::module)
set_target_properties(my_app PROPERTIES CXX_SCAN_FOR_MODULES ON CXX_EXTENSIONS OFF)
```

可从 [独立模块示例](../examples/module/README.md) 开始，示例包含源码接入和安装包接入的完整命令。

GCC 16.1.1 的 `faio.process` 构建在读取核心模块接口时可能报告 `Bad file data`。进程具名模块的验证工具链为 Clang。

### Bazel

使用 Bazel 9.2.0 和支持具名模块的 Clang 发行版，`CC` 指向它的 `clang`。macOS 已验证 Homebrew LLVM 22。在 Linux Docker 中使用容器内的 `export CC=/usr/bin/clang`，并给测试命令加上 `--config=faio_epoll` 以选择 epoll。

```sh
export PATH="$(brew --prefix llvm@22)/bin:$PATH"
export CC="$(command -v clang)"
bazel test --config=faio_headers //...
bazel test --config=faio_modules --config=faio_tls --config=faio_process //...
bazel run --config=faio_modules //examples:quickstart_module
```

头文件库目标为 `//:faio`、`//:faio_tls` 和 `//:faio_process`。模块目标位于 `//cxx-module`，可直接构建 `//cxx-module:faio`、`//cxx-module:tls` 和 `//cxx-module:process`。原有的 `//:faio_module`、`//:faio_module_tls` 和 `//:faio_module_process` 仍可使用。外部 Bzlmod 项目通过 `bazel_dep` 和 `local_path_override` 接入源码，并在自己的根工作区选择工具链和配置。

使用 Bazelisk 或开发镜像时，可以从仓库子目录直接运行 `bazel`。宿主机若直接安装了原生 Bazel，从子目录运行时使用仓库的 `tools/bazel`，将输出定位到仓库根目录的 `target/bazel/`。

### xmake

使用 xmake 3.0.1 或更新版本。macOS 和 Linux 上的 spdlog、TLS 所用 OpenSSL 及 Linux 的 liburing 通过 pkg-config 获取。以下命令在 macOS 选择 Homebrew LLVM 23。

```sh
faio_llvm="$(brew --prefix llvm)"
export PATH="$faio_llvm/bin:$PATH"
xmake f -c -y -m release --toolchain=clang \
  --cxx="$faio_llvm/bin/clang++" --ld="$faio_llvm/bin/clang++" \
  --faio_modules=y --faio_tls=y --faio_process=y
xmake test -j 4
xmake run quickstart_module
```

从仓库根构建且只启用模块时加上 `--faio_headers=n`。可以用 `xmake f -P cxx-module -o target/xmake/cxx-module` 单独配置模块库。Fedora Docker 中用 `--cxx=/usr/sbin/clang++ --ld=/usr/sbin/clang++` 和 `export LD_LIBRARY_PATH=/usr/lib64` 配对系统工具链，必要时用 `--faio_io_uring=n` 选用 epoll。外部源码消费项目通过 `includes("third_party/faio/cxx-module")` 载入模块构建，再用 `add_deps` 链接需要的目标。分发模块归档时随附 `xmake install` 安装的接口源码和模块元数据。

只使用头文件时，保留默认的 `--faio_modules=n`，执行 `xmake build quickstart_header` 和 `xmake run quickstart_header`。

`--faio_benchmarks=y` 注册头文件版基准与 TCP 负载工具。需要 Asio 对照时，同时将 `FAIO_ASIO_INCLUDE_DIR` 设置为 `asio.hpp` 所在目录。

### 实验接口

具名入口 `faio.experimental` 可在 Linux Docker 中用 GCC 16.1、匹配 libstdc++ 和 C++26 构建，提供运行时、反射、生命周期注解和异步入口。CMake 使用 `-DFAIO_MODULE_ENABLE_EXPERIMENTAL=ON` 与 `-DCMAKE_CXX_COMPILER=/opt/gcc-16.1/usr/bin/g++`。Bazel 使用 `CC=/opt/gcc-16.1/usr/bin/gcc` 和 `--config=faio_experimental_modules`。xmake 选择 GCC 工具链，并加上 `--faio_modules=y --faio_module_experimental=y`。

使用反射时先包含 `<meta>` 和其他标准头，再写 `import faio.experimental;`。定义注解式 `main` 的源文件在导入后包含 `<faio-module/entry.hpp>`。

具名模块的 execution 选项默认关闭。仓库锁定的 stdexec 能在纯头文件 C++26 配置运行，在 GCC 16.1 的具名模块中会触发编译器内部错误。需要 sender/receiver 桥接时使用纯头文件版。 [stdexec 上游问题](https://github.com/NVIDIA/stdexec/issues/2148)

### 可重复验证

`scripts/verify_cmake_consumers.py`、`scripts/verify_bazel_consumers.py` 和 `scripts/verify_xmake_consumers.py` 可在缺少另一棵源码树的临时项目中验证消费。CMake 和 xmake 的脚本还会搬迁安装目录，再编译独立的导入方。Linux io_uring 的测试容器需允许对应系统调用。

```sh
python3 scripts/verify_build_organization.py --backend kqueue
python3 scripts/verify_build_organization.py --docker-image faio:dev \
  --tool cmake --compiler clang++-23 --backend all
```

第一个命令在原生 macOS 运行三个构建工具。第二个命令在 Docker 运行 CMake 的 epoll 和 io_uring 验证。Linux 的 Bazel、xmake 验证使用装有对应工具的镜像，并通过 `--tool bazel` 或 `--tool xmake` 选择。

[认识 faio](learn-faio.md) · [API 介绍](api/index.md) · [API 查询](api-reference.md)
