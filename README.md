# faio · Fast Async IO

**简体中文** | [English](README_EN.md)

[项目主页](https://superlxh02.github.io/faio/) · [快速开始](docs/getting-started.md) · [认识 faio](docs/learn-faio.md) · [API 介绍](docs/api/index.md) · [API 查询](https://superlxh02.github.io/faio/api/)

faio 是一个 C++23 异步 I/O 库，默认以头文件形式提供。网络、文件、定时器和同步原语都以协程接口暴露，业务代码直接 `co_await`，调度与 I/O 由运行时处理。Linux 后端支持 epoll 与 io_uring，macOS 使用 kqueue，Windows 使用 IOCP；后端在编译期选定，上层代码无需平台分支。许可证为 Apache-2.0。

## 快速开始

准备 CMake 3.20+、Ninja、spdlog 及平台依赖后，在仓库根目录执行：

```sh
cmake --preset linux-main-release
cmake --build --preset linux-main-release -j4
ctest --preset linux-main-release
./target/cmake-build-linux-main-release/examples/quickstart
```

构建产物统一放在 `target/` 下：CMake 写入 `target/cmake-build-<preset>/`，Bazel 写入 `target/bazel/`，xmake 写入 `target/xmake/`，Rust 对照基准写入 `target/cargo/`。

macOS 把预设换成 `mac-main-release`；Windows 可在 `windows-clang-main-release`、`windows-mingw-main-release`、`windows-msvc-main-release` 中选一个。各平台的工具链准备见[快速开始](docs/getting-started.md)。

## 最小示例

下面这段程序启动两个定时任务，等两者都完成后把结果相加。`block_on` 在调用线程上驱动运行时，直到协程返回：

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

使用模块版时，把 `#include <faio/faio.hpp>` 换成 `import faio;`，其余代码不变。

## Bazel 与 xmake

两套构建系统都同时支持头文件版和 C++20 具名模块版，二者共用 `include/faio/` 的实现。Bazel 锁定 9.2.0，`CC` 需指向支持具名模块的 Clang 发行版：

```sh
export CC="$(command -v clang)"
bazel test --config=faio_headers //...
bazel test --config=faio_modules --config=faio_tls --config=faio_process //...
bazel run --config=faio_modules //examples:quickstart_module
```

xmake 需要 3.0.1 或更新版本。下面的命令在 macOS 上选择 Homebrew LLVM，Linux 开发容器里换成 `/usr/sbin/clang++`：

```sh
faio_cxx="$(brew --prefix llvm)/bin/clang++"
xmake f -c -y -m release --toolchain=clang \
  --cxx="$faio_cxx" --ld="$faio_cxx" --faio_modules=y
xmake test -j 4
xmake run quickstart_module
```

只构建纯头文件版时，Bazel 用 `--config=faio_headers`，xmake 去掉 `--faio_modules=y` 后执行 `xmake build quickstart_header`。目标名、下游接入和已知限制见[模块移植说明](cxx-module/README.md)与[构建与验证命令](docs/getting-started.md#具名模块与其他构建入口)。

## 能力

公开接口按用途分组：

| 能力 | 接口 |
| --- | --- |
| 任务与并发 | `task`、`spawn`、`join`、`select`、`scope` |
| 运行时 | 单线程与多线程调度、协作取消、有界阻塞任务服务 |
| 网络与文件 | TCP、UDP、Unix 网络、异步文件及目录操作 |
| 时间与同步 | 定时器、超时、互斥锁、信号量、条件变量、有界 MPSC |
| 可选组件 | OpenSSL TLS、异步进程 |
| 实验层 | C++26 反射注解入口、stdexec Sender/Receiver 互操作 |

## 构建配置

CMake 的 configure、build、test 使用同名预设，每个预设分 `release` 和 `debug` 两种配置。

| 平台 | 主线 | 全特性 |
| --- | --- | --- |
| Linux | `linux-main-*` | `linux-all-*` |
| macOS | `mac-main-*` | `mac-all-*` |
| Windows clang-cl | `windows-clang-main-*` | `windows-clang-all-*` |
| Windows MinGW | `windows-mingw-main-*` | `windows-mingw-all-*` |
| Windows MSVC | `windows-msvc-main-*` | `windows-msvc-all-*` |

`main` 启用稳定特性，`all` 额外启用实验层。实验层要求 Linux、GCC 16.1.x、匹配的 libstdc++ 16 和 C++26；macOS 和 Windows 的 `all` 预设在配置阶段报告工具链约束。全特性依赖由 `scripts/bootstrap_experimental_deps.py` 准备。

Linux 后端由 `FAIO_ENABLE_IO_URING` 在编译期选择。`FAIO_ENABLE_TLS`、`FAIO_ENABLE_PROCESS` 分别控制 TLS 与异步进程。`FAIO_BUILD_HEADERS`、`FAIO_BUILD_MODULES` 选择构建形态，后者需要 CMake 3.28+；`FAIO_BUILD_EXAMPLES`、`FAIO_BUILD_TESTS`、`FAIO_BUILD_BENCHMARKS` 控制开发目标，`FAIO_INSTALL` 控制安装导出。

## 工程集成

头文件方式最直接：安装 spdlog，把 faio 放进依赖目录，然后：

```cmake
set(FAIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/faio)
target_link_libraries(my_app PRIVATE faio::faio)
```

安装后的消费工程改用 `find_package(faio CONFIG REQUIRED)`。`faio::faio` 目标会传递 C++23、头文件路径、线程库和 spdlog 的链接要求；TLS 与异步进程分别使用 `faio::tls`、`faio::process`。

走模块方式时，构建模块包后使用 `find_package(faioModules CONFIG REQUIRED)` 并链接 `faio::module`，TLS 和进程对应 `faio::module_tls`、`faio::module_process`。Bazel 的下游依赖写作 `//cxx-module:faio`，xmake 写作 `add_deps("faio_module")`。模块对编译器、标准库和构建配置有匹配要求，细节见[模块移植说明](cxx-module/README.md)。

## Docker

仓库提供四个镜像定义：

| 文件 | 用途 |
| --- | --- |
| [docker/dev-image.dockerfile](docker/dev-image.dockerfile) | CLion Docker 工具链环境，系统依赖及备用 vcpkg |
| [docker/duild-all.dockerfile](docker/duild-all.dockerfile) | GCC 16.1，全特性构建 |
| [docker/build-main.dockerfile](docker/build-main.dockerfile) | Clang 23，稳定特性构建，同时安装 GCC |
| [Dockerfile](Dockerfile) | 默认全特性构建 |

开发镜像内置 Bazel 9.2.0 和 xmake 3.0.9，三套构建系统可以直接在容器里使用。镜像命令与配置见 [Docker 说明](docker/README.md)。

## 文档

- [快速开始](docs/getting-started.md)：依赖、平台工具链、构建、模块入口与集成。
- [认识 faio](docs/learn-faio.md)：以一个 TCP 回显服务贯穿十章的渐进教程，另有专题与术语表。
- [API 介绍](docs/api/index.md)：公共头文件、调用方法与生命周期。
- [API 查询](docs/api-reference.md)：按类型、函数和重载检索。
- [文档工具](.github/site/README.md)：本地构建及站点维护。

完整程序在 [examples](examples/README.md)，性能基准在 [benchmark](benchmark/README.md)。

[Apache-2.0](LICENSE)
