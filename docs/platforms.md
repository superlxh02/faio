# 平台与构建

faio 使用 C++20 引入的协程机制，公共构建要求为 **C++23**。库以头文件形式提供，但仍有 spdlog 和线程库等公开链接依赖。

## 平台矩阵

| 平台 | 后端 | 仓库预设 | 依赖与注意事项 |
| --- | --- | --- | --- |
| Linux | io_uring | `linux-clang22-uring` | `FAIO_ENABLE_IO_URING=ON`；需 liburing、Linux 5.10+，具体操作还受内核能力限制 |
| Linux | epoll | `linux-clang22-epoll` | `FAIO_ENABLE_IO_URING=OFF`；无需 liburing |
| macOS | kqueue | `macos-clang23` | Homebrew LLVM；系统 AppleClang 的标准库可能无法通过项目检查 |
| Windows | IOCP | `windows-msvc` / `windows-clang` / `windows-mingw` | 目标 Windows 10/11 x64；固定依赖由 PowerShell bootstrap 准备 |

Linux 后端在编译时固定。运行时 `set_io_backend()` 用于校验所请求的后端与编译结果一致，不能把同一二进制动态切换为另一个后端。

## 工具链要求

仓库 README 记录的验证环境包括 Homebrew Clang 23、Linux Clang 22.1.2 + libstdc++ 15、MSVC 19.51、clang-cl 22.1.0 和 MinGW-w64 GCC 16.2.0-4。请以 [根目录 README](../README.md#环境) 的完整要求及配置期检测为准。

GCC 工具链需要包含协程构造异常清理修复。MinGW 需要 native TLS，项目会拒绝 emutls 工具链。实验性反射层另有更严格要求，见 [实验性特性](实验性特性.md)。

## 构建开关

| CMake 变量 | 默认值 | 用途 |
| --- | --- | --- |
| `FAIO_BUILD_EXAMPLES` | `ON` | 构建官方例程 |
| `FAIO_BUILD_TESTS` | `ON` | 构建测试，需要 GoogleTest |
| `FAIO_BUILD_BENCHMARKS` | `ON` | 构建性能程序，部分需要 standalone Asio |
| `FAIO_INSTALL` | `ON` | 生成安装和 CMake 包导出规则 |
| `FAIO_ENABLE_IO_URING` | Linux 为 `ON`，其余为 `OFF` | 固定选择 Linux I/O 后端 |
| `FAIO_ENABLE_EXPERIMENTAL` | `OFF` | C++26 反射、注解入口及 Sender/Receiver 实验层 |

文档站构建不需要 C++ 编译器或上述库依赖；只需要 Python、Node 和 mdBook，见 [文档维护](contributing-docs.md)。

## 构建与验证例程

例如在 macOS 使用现有预设：

```sh
cmake --preset macos-clang23 -DFAIO_BUILD_EXAMPLES=ON
cmake --build --preset macos-clang23 --target quickstart coroutine_basics -j4
./build/macos-clang23/examples/quickstart
```

启用测试并构建相应程序后，可以执行有限例程测试：

```sh
ctest --test-dir build/macos-clang23 -L examples --output-on-failure
```

常驻 TCP / UDP 服务通过客户端验证，不能直接作为自动结束的命令运行。具体端口、协议和退出方式见 [网络例程](examples/network.md)。
