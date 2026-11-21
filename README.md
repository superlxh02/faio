# faio · Fast Async IO

**简体中文** | [English](README_EN.md)

[项目主页](https://superlxh02.github.io/faio/) · [快速开始](docs/getting-started.md) · [认识 faio](docs/learn-faio.md) · [API 介绍](docs/api/index.md) · [API 查询](https://superlxh02.github.io/faio/api/)

faio 是 C++23 的跨平台异步 I/O 头文件库，基于 C++ 协程实现，采用 Apache-2.0 许可证。Linux 后端支持 epoll 与 io_uring，macOS 使用 kqueue，Windows 使用 IOCP。

| 能力 | 接口 |
| --- | --- |
| 任务与并发 | `task`、`spawn`、`join`、`select`、`scope` |
| 运行时 | 单线程与多线程调度、协作取消、有界阻塞任务服务 |
| 网络与文件 | TCP、UDP、Unix 网络、异步文件及目录操作 |
| 时间与同步 | 定时器、超时、互斥锁、信号量、条件变量、有界 MPSC |
| 可选组件 | OpenSSL TLS、异步进程 |
| 实验层 | C++26 反射注解入口、stdexec Sender/Receiver 互操作 |

## 快速开始

主线工具链为 Clang 23，Windows 还支持 MSVC 和 MinGW-w64。Linux 镜像同时安装 GCC，第三方依赖由系统包提供。准备 CMake 3.20+、Ninja、spdlog 和所需可选依赖后：

```sh
cmake --preset linux-main-release
cmake --build --preset linux-main-release -j4
ctest --preset linux-main-release
./build/linux-main-release/examples/quickstart
```

macOS 使用 `mac-main-release`，Windows 使用 `windows-clang-main-release`、`windows-mingw-main-release` 或 `windows-msvc-main-release`。工具链准备与平台命令见[快速开始](docs/getting-started.md)。

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

## 构建配置

configure、build、test 使用同名预设，每个预设分 `release`、`debug` 两种配置。

| 平台 | 主线 | 全特性 |
| --- | --- | --- |
| Linux | `linux-main-*` | `linux-all-*` |
| macOS | `mac-main-*` | `mac-all-*` |
| Windows clang-cl | `windows-clang-main-*` | `windows-clang-all-*` |
| Windows MinGW | `windows-mingw-main-*` | `windows-mingw-all-*` |
| Windows MSVC | `windows-msvc-main-*` | `windows-msvc-all-*` |

`main` 启用稳定特性，`all` 额外启用实验层。实验层目前要求 Linux、GCC 16.1.x、匹配的 libstdc++ 16 和 C++26；macOS/Windows 的 `all` 预设在配置阶段报告工具链约束。全特性依赖通过 `scripts/bootstrap_experimental_deps.py` 准备。

Linux 后端由 `FAIO_ENABLE_IO_URING` 在编译期选择。TLS、异步进程分别由 `FAIO_ENABLE_TLS`、`FAIO_ENABLE_PROCESS` 控制。`FAIO_BUILD_EXAMPLES`、`FAIO_BUILD_TESTS`、`FAIO_BUILD_BENCHMARKS` 控制开发目标，`FAIO_INSTALL` 控制安装导出。

## 工程集成

先安装 spdlog，再从源码引入：

```cmake
set(FAIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/faio)
target_link_libraries(my_app PRIVATE faio::faio)
```

安装后的消费工程使用 `find_package(faio CONFIG REQUIRED)`。`faio::faio` 目标传递 C++23、头文件、线程库和 spdlog 的链接要求。TLS 使用 `faio::tls`，异步进程使用 `faio::process`。

## Docker

| 文件 | 用途 |
| --- | --- |
| [docker/dev-image.dockerfile](docker/dev-image.dockerfile) | CLion Docker 工具链环境，系统依赖及备用 vcpkg |
| [docker/duild-all.dockerfile](docker/duild-all.dockerfile) | GCC 16.1，全特性构建 |
| [docker/build-main.dockerfile](docker/build-main.dockerfile) | Clang 23，稳定特性构建，同时安装 GCC |
| [Dockerfile](Dockerfile) | 默认全特性构建 |

命令和镜像配置见 [Docker 说明](docker/README.md)。

## 文档

- [快速开始](docs/getting-started.md)：依赖、平台工具链、构建与集成。
- [认识 faio](docs/learn-faio.md)：以一个 TCP 回显服务贯穿十章的渐进教程，另有专题与术语表。
- [API 介绍](docs/api/index.md)：公共头文件、调用方法与生命周期。
- [API 查询](docs/api-reference.md)：按类型、函数和重载检索。
- [文档工具](.github/site/README.md)：本地构建及站点维护。

完整程序在 [examples](examples/README.md)，性能基准在 [benchmark](benchmark/README.md)。

[Apache-2.0](LICENSE)
