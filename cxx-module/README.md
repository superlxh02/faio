# 移植和使用 faio 具名模块

`import faio;` 提供稳定接口，`import faio.tls;`、`import faio.process;` 和 `import faio.experimental;` 是可选入口。模块使用 C++20 的具名模块机制，稳定库按 C++23 编译，接口与纯头文件共用 `include/faio/` 的实现：同一份头文件，两种引入方式。

移植 faio 时建议保留完整仓库，作为子模块或 vendored 依赖。源码构建至少需要 `cxx-module/`、同级的 `include/` 和相应的构建文件；CMake 入口还会用到同级的 `cmake/`。

编译模块会生成 BMI。源码接入由下游工具链生成 BMI；安装接入随包提供重建 BMI 所需的接口源码和共享头。构建归档的程序和消费它的程序必须使用兼容的编译器、标准库与编译配置，并且一个最终程序只能选择头文件或具名模块其中一种接入方式。

| C++ 导入 | CMake 目标 | Bazel 源码目标 | xmake 目标 |
| --- | --- | --- | --- |
| `import faio;` | `faio::module` | `//cxx-module:faio` | `faio_module` |
| `import faio.tls;` | `faio::module_tls` | `//cxx-module:tls` | `faio_module_tls` |
| `import faio.process;` | `faio::module_process` | `//cxx-module:process` | `faio_module_process` |
| `import faio.experimental;` | `faio::module_experimental` | `//cxx-module:experimental` | `faio_module_experimental` |

核心模块链接 spdlog，TLS 需要 OpenSSL 3.5 或更新版本。基础接口和扩展分开导入：先写 `import faio;`，需要扩展时再写 `import faio.tls;` 或 `import faio.process;`。下面各节用的示例程序都是[计时器 quickstart](../examples/module/quickstart.cpp)，运行后输出 `The answer is 42`。

## CMake

需要 CMake 3.28+、Ninja 1.11+ 和支持 C++23 的编译器。用 Clang 时，`clang++` 与 `clang-scan-deps` 必须来自同一份 LLVM 安装，否则扫描阶段会直接失败。macOS 把 Homebrew LLVM 的 `bin` 加入 `PATH`；faio 的开发容器使用 `clang++-23`。

把 faio 放在应用的 `third_party/faio/` 下，用下面的 `CMakeLists.txt` 构建应用：

```cmake
cmake_minimum_required(VERSION 3.28)
project(my_app LANGUAGES CXX)

add_subdirectory(third_party/faio/cxx-module faio-modules)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE faio::module)
set_target_properties(my_app PROPERTIES CXX_SCAN_FOR_MODULES ON CXX_EXTENSIONS OFF)
```

在应用根目录执行构建。示例关掉 io_uring，以便在 Linux 开发容器的默认权限下运行；macOS 用 kqueue，不需要这个开关：

```sh
cmake -S . -B target/cmake-build-module-app -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DFAIO_ENABLE_IO_URING=OFF
cmake --build target/cmake-build-module-app
./target/cmake-build-module-app/my_app
```

要分发模块包，在 faio 仓库根目录构建并安装：

```sh
cmake -S cxx-module -B target/cmake-build-module-provider -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release \
  -DFAIO_ENABLE_IO_URING=OFF
cmake --build target/cmake-build-module-provider
cmake --install target/cmake-build-module-provider --prefix "$PWD/target/faio-module-prefix"
```

安装消费把 `add_subdirectory(...)` 换成 `find_package(faioModules CONFIG REQUIRED)`，配置应用时用 `-DCMAKE_PREFIX_PATH=/path/to/faio-module-prefix` 指向安装前缀，链接目标仍是 `faio::module`。构建提供方时用 `-DFAIO_ENABLE_TLS=ON` 或 `-DFAIO_ENABLE_PROCESS=ON` 打开扩展；消费方在 `find_package` 里加 `COMPONENTS tls`、`COMPONENTS process` 或两者，再链接表中对应的目标。

## Bazel

使用仓库锁定的 Bazel 9.2.0。在 faio 仓库根目录可以直接构建模块目标；已有的 `//:faio_module`、`//:faio_module_tls`、`//:faio_module_process` 和 `//:faio_module_experimental` 标签也仍然可用。

macOS 先把 Homebrew LLVM 22 的 `bin` 加入 `PATH`，再 `export CC="$(command -v clang)"`。Linux 开发容器用 `export CC=/usr/bin/clang`，容器默认权限下再加 `--config=faio_epoll`。

```sh
bazel build --config=faio_modules //cxx-module:faio
bazel run --config=faio_modules //examples:quickstart_module
```

应用通过 Bzlmod 接入时，依赖仓库要保留根目录的 `MODULE.bazel`、`BUILD.bazel`、`REPO.bazel`、`tools/experimental_compiler.cpp`，以及 `include/` 和 `cxx-module/`。在应用的 `MODULE.bazel` 里声明本地源码依赖和工具链配置扩展：

```python
module(name = "my_app")
bazel_dep(name = "faio", version = "0.0.0")
local_path_override(module_name = "faio", path = "third_party/faio")
bazel_dep(name = "rules_cc", version = "0.2.25")

cc = use_extension("@rules_cc//cc:extensions.bzl", "cc_configure_extension")
use_repo(cc, "local_config_cc_toolchains")
```

应用的 `BUILD.bazel` 声明模块依赖：

```python
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

cc_binary(
    name = "my_app",
    srcs = ["main.cpp"],
    deps = ["@faio//cxx-module:faio"],
)
```

应用根目录的 `.bazelrc` 配置扫描与编译，`CC` 仍指向选定的 Clang：

```text
startup --output_user_root=target/bazel/state
common --repo_contents_cache=
build --symlink_prefix=target/bazel/
build:faio_modules --repo_env=CC
build:faio_modules --repo_env=BAZEL_USE_LEGACY_MACOS_TOOLCHAIN=1
build:faio_modules --extra_toolchains=@local_config_cc_toolchains//:all
build:faio_modules --features=-supports_start_end_lib
build:faio_modules --features=cpp_modules
build:faio_modules --experimental_cpp_modules
build:faio_modules --cxxopt=-std=c++23
build:faio_modules --cxxopt=-Xclang=-fmodule-file-home-is-cwd
build:faio_modules --cxxopt=-Xclang=-fmodules-embed-all-files
```

然后在应用根目录执行 `bazel run --config=faio_modules //:my_app`。Linux 上用 `--@faio//:io_uring=off` 选择 epoll；需要 TLS 或进程时，分别加 `--@faio//:tls=true`、`--@faio//:process=true`，并把 `@faio//cxx-module:tls` 或 `@faio//cxx-module:process` 加进 `deps`。

## xmake

需要 xmake 3.0.1 或更新版本，以及能通过 pkg-config 找到的 spdlog。macOS 先选择 Homebrew LLVM：

```sh
faio_cxx="$(brew --prefix llvm)/bin/clang++"
export PATH="$(dirname "$faio_cxx"):$PATH"
```

在 faio 的 Fedora 开发容器里改用系统 Clang 和标准库：

```sh
faio_cxx=/usr/sbin/clang++
export LD_LIBRARY_PATH="/usr/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

在 faio 仓库根目录单独构建模块库：

```sh
export XMAKE_CONFIGDIR="$PWD/target/xmake/config-module"
xmake f -P cxx-module -c -y -m release -o target/xmake/cxx-module --toolchain=clang \
  --cxx="$faio_cxx" --ld="$faio_cxx" --faio_io_uring=n
xmake build -P cxx-module faio_module
```

应用从源码消费时，把 faio 放在 `third_party/faio/`，模块入口会加载仓库的 `tools/xmake/common.lua`。应用的 `xmake.lua` 这样写：

```lua
set_project("my_app")
set_languages("c++23")
includes("third_party/faio/cxx-module")

target("my_app")
    set_kind("binary")
    set_policy("build.c++.modules", true)
    set_policy("build.c++.modules.std", false)
    add_files("main.cpp")
    add_deps("faio_module")
```

在应用根目录配置并运行，编译器沿用上面选定的路径：

```sh
export XMAKE_CONFIGDIR="$PWD/target/xmake/config-app"
xmake f -P . -c -y -m release -o target/xmake --toolchain=clang \
  --cxx="$faio_cxx" --ld="$faio_cxx" --faio_io_uring=n
xmake build -P . my_app
xmake run -P . my_app
```

需要 TLS 或进程时，配置 `--faio_tls=y` 或 `--faio_process=y`，并把对应的库目标加进 `add_deps`。也可以用 `xmake install -P cxx-module -o "$PWD/target/faio-module-prefix" faio_module` 分发核心归档，安装前缀同时包含接口源码、模块元数据和共享头。通过 xmake 包消费时保留完整前缀，让下游打开 `build.c++.modules` 重建 BMI，并把 spdlog 等系统依赖加进下游目标。本地包接入的完整验证见 [verify_xmake_consumers.py](../scripts/verify_xmake_consumers.py)。

## 实验接口

`import faio.experimental;` 需要 Linux、GCC 16.1.x、匹配的 libstdc++ 16 和 C++26。用到 `std::meta` 或反射运算的源文件先包含 `<meta>`，再导入实验模块；使用异步入口宏时，在导入后包含 `<faio-module/entry.hpp>`。稳定的 quickstart 只依赖核心模块、计时器和标准库。

实验模块的 execution 选项默认关闭：仓库锁定的 stdexec 在 GCC 16.1 的具名模块里会触发编译器内部错误。需要 Sender/Receiver 桥接时改用纯头文件版。

## 已知限制

GCC 16.1.1 构建 `faio.process` 模块时，可能在读取核心模块接口时报 `Bad file data`，进程模块的验证工具链是 Clang。CMake 构建需要 Ninja 1.11+；用 AppleClang 时 CMake 无法扫描具名模块，要换成 Homebrew LLVM 的 `clang++`。

实验开关、平台工具链和验证命令见[构建指南](../docs/getting-started.md#具名模块与其他构建入口)。
