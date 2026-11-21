# 环境准备

这一章我们把工具链装好，建一个能编译 faio 的 CMake 工程，再跑通一个最小的程序。等到最后，你会看到终端打印出 `The answer is 42`。

## faio 需要什么

faio 是 header-only 库，不需要预先编译出 `.a` 或 `.so`。但它用到了一些比较新的语言特性，所以编译器版本有要求：

- C++23 标准；
- 支持协程、`std::expected`、`std::format`，以及显式对象参数（即 `this` 参数）；
- CMake 3.20+ 与 Ninja。

CMake 在配置阶段会编译一小段代码来检测这些能力。缺了哪一项，配置会直接报错并告诉你是哪一个，不会拖到链接阶段才出问题。

三个平台的工具链和依赖如下：

| 平台 | 主线工具链 | 依赖 |
| --- | --- | --- |
| Linux | Clang 23，同时装 GCC | spdlog、OpenSSL、liburing；完整开发构建另需 GoogleTest 和 standalone Asio |
| macOS | Homebrew LLVM Clang 23，同时装 GCC | `brew install llvm gcc cmake ninja spdlog openssl googletest asio` |
| Windows | MSVC、clang-cl 或 MinGW-w64 | 用 `scripts/bootstrap_windows.ps1` 准备固定版本依赖 |

Linux 用户可以按[Docker 构建说明](../../docker/README.md)直接拉一个配好环境的镜像。依赖由系统包全局安装，CMake 配置阶段不联网下载。

## 拿到源码

把仓库克隆下来，然后进入根目录。后面所有命令都从这里执行。

```sh
git clone https://github.com/superlxh02/faio.git
cd faio
```

## 建一个自己的工程

先别急着改 faio 里的例子。我们建一个独立的 `my_app`，这样你能更清楚地看到集成一个头文件库到底要做哪些事。

目录结构是这样的：

```text
my_app/
├── CMakeLists.txt
└── main.cpp
```

`CMakeLists.txt` 用 `add_subdirectory` 把 faio 挂进来，然后链接公共目标 `faio::faio`：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)

set(FAIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FAIO_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/faio)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE faio::faio)
```

那三个 `set(... OFF ...)` 是关掉 faio 自带的示例、测试和基准。我们只要库本身，没必要编译那些东西。

把 faio 放到 `my_app/third_party/faio` 下面。如果你克隆的位置不同，改一下 `add_subdirectory` 的路径即可。

## 写下第一个程序

在 `main.cpp` 里放入一个小任务：等 10 毫秒，然后返回一个数字。

```cpp
#include <faio/faio.hpp>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

faio::task<int> delayed_value(int value) {
  co_await faio::time::sleep(10ms);
  co_return value;
}

int main() {
  const auto result = faio::block_on(delayed_value(42));
  std::cout << "The answer is " << result << '\n';
  return 0;
}
```

先混个脸熟，这几个东西后面会反复出现：

- `faio::task<int>` 是一个异步任务的返回类型。调用 `delayed_value(42)` 只是创建了任务，函数体还没跑。
- `co_await` 在这里等待一个定时器。等待期间当前协程挂起，线程可以去做别的事。
- `faio::block_on()` 在普通线程里驱动任务，直到它返回结果。它是同步世界和异步世界之间的桥。

## 编译和运行

在 Linux 上用主线预设配置、构建，然后运行。这里先关掉测试和基准，让第一次构建快一些。

```sh
cmake --preset linux-main-release \
  -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset linux-main-release --target quickstart
./build/linux-main-release/examples/quickstart
```

如果你用的是上面独立的 `my_app` 工程，就直接在工程目录里执行普通的 CMake 流程：

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/my_app
```

macOS 需要先把 Homebrew LLVM 的 bin 加到 PATH，再用 `mac-main-release` 预设。Windows 在 PowerShell 里先运行 `scripts/bootstrap_windows.ps1`，再用 `scripts/windows_environment.ps1 msvc`（或 `clang`、`mingw`）选好工具链，然后用对应的预设。各平台的完整命令见[快速开始](../getting-started.md)。

不管用哪种方式，跑起来之后你应该看到：

```text
The answer is 42
```

## 配置运行时

faio 的默认运行时是多线程的。如果想改成单线程，或者指定 worker 数量，可以在第一次 `block_on()` 或 `spawn()` 之前配置一次：

```cpp
faio::runtime::configure(faio::config_builder{}
                             .set_mode(faio::runtime::mode::multi_thread)
                             .set_num_workers(2)
                             .build());
```

`config_builder` 在 `faio` 命名空间下，而 `mode` 在 `faio::runtime` 下，写的时候留意一下。`build()` 返回的是配置的副本，之后修改 builder 不会影响已经启动的运行时。

配置必须在第一次驱动任务之前完成。一旦 `block_on()` 或 `spawn()` 跑起来，再改配置就不生效了。更多选项见[运行时与配置](../api/runtime.md)。

## 卡住了怎么办

编译器版本、依赖路径、预设名称，这几个是最容易出问题的地方。可以先对照[快速开始](../getting-started.md)里的表格逐项检查。仍然是谜题的话，把完整的报错贴到 GitHub Issues，通常一看就知道是哪一步漏了。

---

[上一章：认识 faio](../learn-faio.md) · [下一章：Hello faio →](hello-faio.md)
