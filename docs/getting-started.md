# 快速开始

这一章从零构建 faio，并运行第一个程序 `examples/quickstart.cpp`：它组合一个任务，等待 10 毫秒后输出 `Hello, world!`。

先克隆源码，后面的命令都在仓库根目录执行：

```sh
git clone https://github.com/superlxh02/faio.git
cd faio
```

## 编译器要求

主线使用 C++23、CMake 3.20+ 和 Ninja。编译器要支持协程、`std::expected`、`std::format` 和显式对象参数，兼顾这几项的最低版本如下：

| 编译器 | 最低版本 |
| --- | --- |
| GCC | 14 |
| Clang | 20 |
| MSVC | 19.43（Visual Studio 2022 17.13） |

CMake、Bazel 和 xmake 使用同一套编译器要求；Bazel 9.2.0 和 xmake 3.0.1+ 的版本要求见各自章节。

## CMake 构建

### 依赖

CMake 通过 `find_package` 从系统包或 vcpkg 查找依赖。

**Linux**

Debian / Ubuntu：

```sh
sudo apt install -y libspdlog-dev liburing-dev libgtest-dev libasio-dev libgoogle-perftools-dev libssl-dev
```

Red Hat / Fedora：

```sh
sudo dnf install -y spdlog-devel liburing-devel gtest-devel asio-devel gperftools-devel openssl-devel
```

**macOS**

推荐用 vcpkg。在仓库根目录执行（`VCPKG_ROOT` 指向本机 vcpkg）：

```sh
"$VCPKG_ROOT/vcpkg" install --triplet arm64-osx --x-feature=protocols
```

Apple Silicon 用 `arm64-osx`，Intel 用 `x64-osx`。

**Windows**

同样用 vcpkg。在仓库根目录的 PowerShell 中执行：

```powershell
& "$env:VCPKG_ROOT\vcpkg.exe" install --triplet x64-windows --x-feature=protocols
```

MinGW 使用 `x64-mingw-static` 或 `x64-mingw-dynamic`，并让后面的 `VCPKG_TARGET_TRIPLET` 与之一致。

TLS 组件要求 OpenSSL 3.5 及以上。下面的构建默认启用 TLS，所以 vcpkg 安装要带 `--x-feature=protocols`；走系统包时请确认 OpenSSL 版本，不足 3.5 时自行安装或用 vcpkg。

不想在本机装依赖，也可以按 [Docker 构建说明](../docker/README.md)使用现成的 Linux 镜像。

### 构建和编译

使用仓库已有的 CMake 预设配置并编译全部常规目标：库、示例和基准。测试关闭；实验层由 `main` 预设关闭。具名模块需要 CMake 3.28+ 和模块工具链，单独放在后面的章节；需要连模块一起编时，去掉 `-DFAIO_BUILD_MODULES=OFF`。

**Linux**

```sh
cmake --preset linux-main-release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DFAIO_BUILD_MODULES=OFF -DFAIO_BUILD_TESTS=OFF
cmake --build --preset linux-main-release
```

`linux-main-*` 预设默认指向开发镜像里的 `clang-23`/`clang++-23`；上面的 `-DCMAKE_*_COMPILER` 把它换成 PATH 中的 `clang`/`clang++`，也可以改成你安装的版本。TLS、进程、io_uring 和基准保持开启，依赖分别来自 OpenSSL、liburing 和 tcmalloc。发行版自带的 OpenSSL 低于 3.5 时 `find_package(OpenSSL 3.5)` 会失败，需要改用 vcpkg 安装 `--x-feature=protocols` 并加上面 macOS 那样的工具链参数，或自行安装 3.5+。

**macOS**

```sh
export PATH="$(brew --prefix llvm)/bin:$PATH"
cmake --preset mac-main-release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DFAIO_BUILD_MODULES=OFF -DFAIO_BUILD_TESTS=OFF
cmake --build --preset mac-main-release
```

Intel Mac 把 `arm64-osx` 换成 `x64-osx`。`mac-main-*` 预设已经设置 `FAIO_USE_WORKSPACE_DEPS=OFF`，vcpkg 工具链会按 `vcpkg.json` 查找 spdlog 和 OpenSSL；TLS、进程和基准保持开启。

**Windows**

在 Visual Studio 开发者 PowerShell 中运行：

```powershell
cmake --preset windows-msvc-main-release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DFAIO_USE_WORKSPACE_DEPS=OFF `
  -DFAIO_BUILD_MODULES=OFF -DFAIO_BUILD_TESTS=OFF
cmake --build --preset windows-msvc-main-release
```

clang-cl 和 MinGW 分别换成 `windows-clang-main-release`、`windows-mingw-main-release`。clang-cl 继续用 `x64-windows`；MinGW 用 `x64-mingw-static` 或 `x64-mingw-dynamic`。`windows-*` 预设默认从 `../faio-deps` 取依赖，上面的 `FAIO_USE_WORKSPACE_DEPS=OFF` 改为使用 vcpkg；MinGW 预设的编译器路径也指向 `../faio-deps`，本机没有这棵目录时，同时用 `-DCMAKE_C_COMPILER`、`-DCMAKE_CXX_COMPILER` 指向自己的 `gcc.exe`、`g++.exe`。TLS 和进程保持开启，OpenSSL 由 vcpkg 的 `protocols` 特性提供。

#### CMake 预设

`CMakePresets.json` 里的 configure、build、test 三组预设同名，按 `<平台>-<特性>-<构建类型>` 命名，二进制目录统一是 `target/cmake-build-<preset>/`。

| 平台 | 主线预设 | 全特性预设 |
| --- | --- | --- |
| Linux | `linux-main-release/debug` | `linux-all-release/debug` |
| macOS | `mac-main-release/debug` | `mac-all-release/debug` |
| Windows clang-cl | `windows-clang-main-release/debug` | `windows-clang-all-release/debug` |
| Windows MinGW | `windows-mingw-main-release/debug` | `windows-mingw-all-release/debug` |
| Windows MSVC | `windows-msvc-main-release/debug` | `windows-msvc-all-release/debug` |

表中写成 `release/debug` 的条目代表两个独立预设，例如 `linux-main-release` 和 `linux-main-debug`。`main` 只开稳定特性；`all` 额外打开实验层，当前要求 **Linux + GCC 16.1.x + libstdc++ 16 + C++26**，macOS 和 Windows 请使用 `main`。

Linux 和 macOS 的 `*-main-*` 预设继承隐藏预设 `modules`，即 `FAIO_BUILD_MODULES=ON`；Windows 的 main 预设不打开模块。上面的全量构建用 `-DFAIO_BUILD_MODULES=OFF` 保持头文件形态；需要 C++20 具名模块时保持开启，并按后面的模块章节准备工具链。

configure、build、test 分别这样用：

```sh
cmake --preset linux-main-debug
cmake --build --preset linux-main-debug -j4
ctest --preset linux-main-debug
```

跑测试需要 GoogleTest，Debug 构建把 `release` 换成 `debug`。构建矩阵和外部消费验证脚本见 [scripts/README.md](../scripts/README.md)。

### 运行

Linux：

```sh
./target/cmake-build-linux-main-release/examples/quickstart
```

macOS 把 `linux` 换成 `mac`。Windows：

```powershell
.\target\cmake-build-windows-msvc-main-release\examples\quickstart.exe
```

三种构建方式输出同一行结果：

```text
Hello, world!
```

程序正常退出。

## Bazel 构建

### 依赖

Bazel 通过 `MODULE.bazel` 声明依赖：大部分来自 BCR（Bazel Central Registry），实验和基准依赖用 `http_archive`。首次构建自动下载，不需要预装 spdlog、gtest 或 vcpkg，也不需要 xrepo 这类外部包管理器。版本锁在 `MODULE.bazel`；查看依赖图：

```sh
bazel mod graph
```

### 构建和编译

使用仓库锁定的 Bazel 9.2.0，版本写在 `.bazelversion`，Bazelisk 会自动选择。

macOS：

Bazel 9 目前会被 Homebrew LLVM 23 的 SDK 依赖检查拦住（`SDKSettings.json` 被当成绝对路径头文件），改用 LLVM 22：

```sh
export PATH="$(brew --prefix llvm@22)/bin:$PATH"
export CC="$(command -v clang)"
bazel build --config=faio_headers --config=faio_tls --config=faio_process \
  -- //... //benchmark:all -//tests/...
```

Linux：

```sh
export CC="$(command -v clang)"
bazel build --config=faio_headers --config=faio_tls --config=faio_process \
  -- //... //benchmark:all -//tests/...
```

`--config=faio_headers` 选择纯头文件库和 C++23，`--config=faio_tls`、`--config=faio_process` 打开 TLS 和进程。这里不传 `--config=faio_epoll`：Linux 默认使用 io_uring，macOS 自动退回 kqueue。`//...` 编译全部常规目标和示例，`-//tests/...` 跳过测试，`//benchmark:all` 把标记为 `manual` 的基准也纳入。具名模块目标在没有模块配置时不兼容，会被自动跳过；需要模块时用后面的 `--config=faio_std_modules`。Windows 上用 Bazel 需要自行准备 clang-cl 工具链，本章的 Windows 快速开始走 CMake。

### 运行

```sh
bazel run --config=faio_headers --config=faio_tls --config=faio_process //examples:quickstart
```

macOS 沿用上面 LLVM 22 的 `PATH` 和 `CC`；Linux 先执行 `export CC="$(command -v clang)"`。输出同上。

## XMake 构建

### 依赖

| 平台 | 安装方式 |
| --- | --- |
| Linux | Debian/Ubuntu：`sudo apt install -y libspdlog-dev liburing-dev libssl-dev libgoogle-perftools-dev`；Red Hat/Fedora：`sudo dnf install -y spdlog-devel liburing-devel openssl-devel gperftools-devel` |
| macOS | 推荐 vcpkg：`"$VCPKG_ROOT/vcpkg" install --triplet arm64-osx --x-feature=protocols`；只用 spdlog 时可 `brew install spdlog` |
| Windows | `& "$env:VCPKG_ROOT\vcpkg.exe" install --triplet x64-windows --x-feature=protocols` |

TLS 需要 OpenSSL 3.5 及以上；发行版版本不足时用 vcpkg 或自行安装 3.5+。

macOS 用 vcpkg 时，构建前按下面的命令设置 `PKG_CONFIG_PATH`。

xrepo 是 xmake 的包管理器；也可以直接用 xrepo 提供 spdlog 和 OpenSSL：

```sh
xrepo install spdlog openssl
xrepo env -b "spdlog,openssl" bash
```

在打开的 shell 里执行下面的 xmake 命令。Windows 上 xmake 的 `vcpkg::` provider 会读取 `VCPKG_ROOT`；macOS 用 vcpkg 时通过 `PKG_CONFIG_PATH` 找到包（见下面的构建命令）。

### 构建和编译

使用 xmake 3.0.1 或更新版本。

macOS / Linux：

```sh
xmake f -c -y -m release --toolchain=clang \
  --cxx="$(command -v clang++)" --ld="$(command -v clang++)" \
  --faio_tls=y --faio_process=y --faio_benchmarks=y \
  --faio_modules=n --faio_tests=n
xmake build
```

macOS 上先执行 `export PATH="$(brew --prefix llvm)/bin:$PATH"`。用 vcpkg 安装依赖时，再把 pkg-config 路径指到 vcpkg：

```sh
export PKG_CONFIG_PATH="$VCPKG_ROOT/installed/arm64-osx/lib/pkgconfig:$PKG_CONFIG_PATH"
```

Linux 用发行版包时不需要这一步，Intel Mac 把 `arm64-osx` 换成 `x64-osx`。

Windows（PowerShell）：

```powershell
xmake f -c -y -m release --toolchain=msvc `
  --faio_tls=y --faio_process=y --faio_benchmarks=y `
  --faio_modules=n --faio_tests=n
xmake build
```

`xmake build` 不指定目标时，会编译全部已启用的目标：头文件库、示例和基准。测试和具名模块关闭，实验层保持默认关闭；Linux 的 io_uring 按 auto 打开，macOS 自动关闭。

xmake 的 `vcpkg::` provider 会读取 `VCPKG_ROOT`，先确认本机已设置该变量并完成上面的 vcpkg 安装。

### 运行

```sh
xmake run quickstart_header
```

输出同上。

## 示例程序

示例通过 `<faio/faio.hpp>` 拿到全部公开接口。其他常用入口见 [API 介绍](api/index.md#包含哪个头文件)。

```cpp
{{#include ../examples/quickstart.cpp}}
```

`task<void>` 定义一个惰性协程；`run()` 用 `co_await faio::join(hello())` 组合并等待这个任务，`main` 再用 `block_on()` 驱动整个任务直到完成。

## C++20 模块集成和使用

具名模块把稳定接口打包成 `import faio;`；TLS 和进程是可选入口：`import faio.tls;`、`import faio.process;`。`include/faio/` 是头文件版和模块版共用的实现，每个最终程序选择一种消费方式。

模块构建要求 CMake 3.28+、Ninja 1.11+ 和支持具名模块的 Clang 或 GCC；Clang 还需要同发行版的 `clang-scan-deps`，macOS 使用 Homebrew LLVM，Apple Clang 不能扫描具名模块。`examples/module` 默认 `import std;`，需要 GCC 16.1+ 和 libstdc++ 提供的 `libstdc++.modules.json`。

### 源码集成

在仓库根目录构建独立模块示例：

```sh
cmake -S examples/module -B target/cmake-build-module-example -G Ninja \
  -DCMAKE_CXX_COMPILER=g++-16 -DCMAKE_BUILD_TYPE=Release
cmake --build target/cmake-build-module-example
./target/cmake-build-module-example/quickstart_module
```

在自己的 CMake 工程里，把整个 faio 源码放到 `third_party/faio`，然后加入模块目录并链接 `faio::module`：

```cmake
cmake_minimum_required(VERSION 3.28)
project(my_app LANGUAGES CXX)

add_subdirectory(third_party/faio/cxx-module faio-modules)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE faio::module)
set_target_properties(my_app PROPERTIES
    CXX_SCAN_FOR_MODULES ON
    CXX_EXTENSIONS OFF)
```

`main.cpp` 里直接写 `import faio;`。如果还要 `import std;`，在 `project()` 之前包含 `third_party/faio/cmake/FaioImportStd.cmake`，并对目标调用 `faio_enable_import_std(my_app)`；独立示例已经包含这一步。

### 安装包接入

默认只安装核心模块；需要 TLS 或进程组件时，在配置命令中加 `-DFAIO_ENABLE_TLS=ON` 或 `-DFAIO_ENABLE_PROCESS=ON`（TLS 还要求 OpenSSL 3.5+）。没有启用的组件不会出现在安装包中。

先构建并安装模块包：

```sh
cmake -S cxx-module -B target/cmake-build-module-provider -G Ninja \
  -DCMAKE_CXX_COMPILER=g++-16 -DCMAKE_BUILD_TYPE=Release \
  -DFAIO_ENABLE_IO_URING=OFF
cmake --build target/cmake-build-module-provider
cmake --install target/cmake-build-module-provider --prefix "$PWD/target/faio-module-prefix"
```

消费方用 `find_package(faioModules CONFIG REQUIRED)`，把 `CMAKE_PREFIX_PATH` 指向上面的安装前缀，链接目标仍是 `faio::module`：

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++-16 \
  -DCMAKE_PREFIX_PATH="$PWD/target/faio-module-prefix"
```

```cmake
find_package(faioModules CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE faio::module)
set_target_properties(my_app PROPERTIES CXX_SCAN_FOR_MODULES ON CXX_EXTENSIONS OFF)
```

启用了 TLS 和进程组件的安装包，用 `find_package(faioModules CONFIG REQUIRED COMPONENTS tls process)` 获取对应目标，分别链接 `faio::module_tls`、`faio::module_process`。消费方必须使用与构建模块时相同的编译器、标准库和构建配置；安装包会带上接口源码，由下游工具链重建 BMI。

### Bazel 和 xmake

Bazel 用 `faio_std_modules` 配置构建同一示例（Linux，GCC 16.1+）：

```sh
export CC="$(command -v g++-16)"
bazel run --config=faio_std_modules //examples:quickstart_module
```

xmake 选择 GCC 并显式给出三个 driver：

```sh
xmake f -c -y -m release --toolchain=gcc \
  --cc="$(command -v gcc-16)" --cxx="$(command -v g++-16)" --ld="$(command -v g++-16)" \
  --faio_modules=y
xmake run quickstart_module
```

目标名、下游接入和已知限制见[模块移植说明](../cxx-module/README.md)。GCC 16.1.1 的 `faio.process` 在读取核心模块接口时可能报 `Bad file data`，进程模块的验证工具链是 Clang。

实验入口 `import faio.experimental;` 要求 Linux + GCC 16.1 + C++26，默认关闭；开关和示例见[模块移植说明](../cxx-module/README.md#实验接口)与[实验性接口](api/experimental.md)。

[认识 faio](learn-faio.md) · [API 介绍](api/index.md) · [API 查询](api-reference.md)
