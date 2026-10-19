# 快速开始

目标：准备依赖，构建 `examples/quickstart.cpp`，运行得到 `42`。以下命令均从仓库根目录执行。

## 1. 准备工具链

基础库需要 C++23、CMake 3.20+、线程库和 spdlog。使用 Ninja 时还需安装 Ninja。CMake 会编译检测协程、`std::expected` 和 `std::format`，仅设置 `-std=c++23` 并不能补齐标准库支持。

- **macOS**：使用 Homebrew LLVM，项目预设为 `macos-clang23`。安装 `brew install llvm cmake ninja spdlog`，确认 `/opt/homebrew/opt/llvm/bin/clang++` 可用。Intel Mac 或不同安装位置请显式设置编译器路径。
- **Linux**：使用满足项目要求的 Clang / GCC 和标准库，准备 CMake、Ninja、spdlog。第一次体验推荐关闭 io_uring，使用 epoll，省去 liburing 依赖。
- **Windows**：先执行 `scripts/bootstrap_windows.ps1` 准备固定版本依赖，再使用 `windows-msvc`、`windows-clang` 或 `windows-mingw` 预设。

工具链的已验证版本、GCC 异常清理要求和系统差异见 [平台与构建](platforms.md)。CMake 配置阶段不会下载第三方依赖。

## 2. 获取并配置

```sh
git clone https://github.com/superlxh02/faio.git
cd faio
```

macOS（Apple Silicon）：

```sh
cmake -S . -B build/local -G Ninja \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DFAIO_BUILD_EXAMPLES=ON \
  -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
```

Linux（以已安装的 Clang 22 为例）：

```sh
cmake -S . -B build/local -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-22 \
  -DFAIO_ENABLE_IO_URING=OFF -DFAIO_BUILD_EXAMPLES=ON \
  -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
```

Windows（在所选工具链的终端中运行）：

```powershell
pwsh -File scripts/bootstrap_windows.ps1
cmake --preset windows-msvc -DFAIO_BUILD_TESTS=OFF -DFAIO_BUILD_BENCHMARKS=OFF
cmake --build --preset windows-msvc --target quickstart
.\build\windows-msvc\examples\quickstart.exe
```

## 3. 构建并运行

macOS / Linux：

```sh
cmake --build build/local --target quickstart -j4
./build/local/examples/quickstart
```

预期标准输出：

```text
The answer is 42
```

## 4. 理解完整程序

```cpp
{{#include ../examples/quickstart.cpp}}
```

1. `task<int>` 声明一个返回整数的惰性协程任务。
2. `sleep(10ms)` 挂起协程，工作线程可以执行其他任务。
3. `spawn()` 立即提交任务并返回结果句柄，`co_await` 该句柄即等待它完成。
4. `join()` 在被等待时启动两个任务，结果顺序与参数顺序一致。
5. `block_on()` 等待当前任务组完成，然后在普通线程取回结果。
6. `runtime::shutdown()` 排空并关闭默认运行时；关闭之后不能重新提交任务。

## 5. 集成到自己的工程

把 faio 放在 `third_party/faio`，提前安装好 spdlog：

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

`faio::faio` 会传递 C++23、头文件路径、线程库与 spdlog 的链接要求。Linux 如果选用 epoll，在配置时额外传入 `-DFAIO_ENABLE_IO_URING=OFF`；选择 io_uring 则需预先安装 liburing。

安装导出与消费端的 `find_package(faio CONFIG REQUIRED)` 用法见 [根目录 README](../README.md#集成)。

## 下一步

[理解任务与生命周期](concepts.md) · [运行完整例程](examples/index.md) · [查阅接口](api/index.md)
