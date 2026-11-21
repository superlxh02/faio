# Docker 工具链

在仓库根目录执行命令，构建上下文为 `.`。镜像支持 aarch64 和 x86_64。

| 文件 | 用途 | 编译器 |
|---|---|---|
| `dev-image.dockerfile` | CLion 开发、调试和依赖环境 | Clang 23、GCC 16.1、clangd 及锁定 stdexec |
| `duild-all.dockerfile` | 全特性构建、安装，包含实验组件 | GCC 16.1 |
| `build-main.dockerfile` | 稳定特性构建、安装 | Clang 23，同时安装 GCC 16.1 |
| 根目录 `Dockerfile` | 与 `duild-all.dockerfile` 内容一致 | GCC 16.1 |

spdlog、GoogleTest、Asio、OpenSSL、liburing 和 gperftools 由 Fedora 包管理器全局安装。vcpkg 安装在 `/opt/vcpkg`，供调用方备用；镜像构建不使用 vcpkg 安装项目依赖。实验用 stdexec 按项目锁文件获取到 `/opt/faio-deps/src/stdexec`，CMake 配置阶段不下载依赖。

GCC 及配套运行库来自 SHA256 固定的 Fedora `16.1.1-2.fc44` RPM，安装到 `/opt/gcc-16.1`。`duild-all.dockerfile` 与根 Dockerfile 的 LLVM 可执行工具只有 clangd，另带它所需的运行库，不安装 Clang 编译器、clang-tidy、LLD 或 LLDB。开发镜像和稳定特性构建镜像提供 Clang 23 与 GCC 16.1，Clang 使用匹配的 GCC 16.1 标准库。开发镜像可在同一 CLion 工具链中选择 all 或 main 预设，普通 CMake 配置默认不启用实验组件。

```sh
docker build -f docker/dev-image.dockerfile -t faio:dev .
docker build -f docker/duild-all.dockerfile -t faio:all .
docker build -f docker/build-main.dockerfile -t faio:main .
docker build -t faio:all .
```

构建镜像编译所有 C++ 示例、单元测试、性能基准及三个锁定的 Tokio 对照程序，并安装 faio 包。`BUILD_TYPE=Release|Debug` 与 `BUILD_JOBS` 控制构建类型和并发度，默认 Release、4 个任务。镜像构建阶段不运行依赖宿主内核的 I/O 测试。

```sh
docker build -f docker/build-main.dockerfile --build-arg BUILD_TYPE=Debug --build-arg BUILD_JOBS=2 -t faio:main-debug .
docker run --rm --security-opt seccomp=unconfined faio:all ctest --preset linux-all-release
docker run --rm -it -v "$PWD:/workspace/faio" -w /workspace/faio faio:dev
```

io_uring 运行需要宿主内核及容器安全配置允许对应的系统调用。预设也可通过 `-DFAIO_ENABLE_IO_URING=OFF` 选择 epoll。`BASE_IMAGE` 可覆盖默认的 `fedora:45`，替代镜像须提供 Clang 23 和所需依赖；`KOJI_ADDRESS` 可覆盖单次构建时官方 RPM 源的 DNS 解析，HTTPS 证书仍按域名校验。

## CMake 预设

Linux 使用 `linux-{all,main}-{release,debug}`，macOS 使用 `mac-{all,main}-{release,debug}`，Windows 使用 `windows-{clang,mingw,msvc}-{all,main}-{release,debug}`；configure、build、test 预设名称一致。

`all` 显式请求全部实验组件。当前实验实现要求 Linux、GCC 16.1 及匹配的 libstdc++；macOS 和 Windows 的 `all` 请求会在配置阶段报告该限制，`main` 用于这些平台的稳定构建。macOS main 需将 Homebrew LLVM 23 的 bin 加入 PATH，并设置 Homebrew 依赖前缀。Windows main 分别使用 clang-cl、MinGW-w64 和 MSVC，依赖由各编译器独立构建。

[CLion 配置](CLion.md)

工具链来源：[Fedora GCC](https://packages.fedoraproject.org/pkgs/gcc/gcc/)、[Fedora Clang 23](https://packages.fedoraproject.org/pkgs/llvm/clang/fedora-45.html)、[clangd 包](https://packages.fedoraproject.org/pkgs/llvm/clang-tools-extra/)、[vcpkg 固定发行版](https://github.com/microsoft/vcpkg/releases/tag/2026.10.16)。

## Linux 分配器

`FAIO_ALLOCATOR=AUTO|SYSTEM|TCMALLOC` 选择进程分配器。AUTO 在 Linux 检测到 tcmalloc 且未启用检测器时使用 tcmalloc；其他平台及检测器构建使用系统分配器。显式 TCMALLOC 要求 Linux 和可用的 gperftools，不能与检测器混用。

链接 tcmalloc 会替换进程的 malloc/new。协程帧缓存及大帧池配置保持有效，`FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW` 默认 OFF。安装包的消费项目按同一选项重新解析分配器，SYSTEM 不要求 gperftools。检测覆盖公共 CMake flags 和配置专用 flags；包导入时无法预知后续 `target_compile_options` 添加的检测器。手动给最终目标添加检测器时，消费项目应显式设置 `-DFAIO_ALLOCATOR=SYSTEM`，或通过 `FAIO_SANITIZERS` 声明检测器配置。
