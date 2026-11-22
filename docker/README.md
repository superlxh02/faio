# Docker 工具链

这几份镜像解决同一个问题：机器上没有 GCC 16.1 和 Clang 23，也不打算把它们连同一堆系统库装到本机。日常开发用开发镜像，需要一份装好的 faio 安装包时用另外两份。所有命令都在仓库根目录执行，构建上下文固定为 `.`，镜像同时支持 x86_64 和 aarch64。

| Dockerfile | 编译器 | 用途 |
|---|---|---|
| `dev-image.dockerfile` | Clang 23，另装 GCC 16.1 | CLion 开发、调试和依赖环境，也可直接当 dev container |
| `duild-all.dockerfile` | GCC 16.1 | 全特性构建、安装，含实验组件 |
| `build-main.dockerfile` | Clang 23，另装 GCC 16.1 | 稳定特性构建、安装 |

根目录的 `Dockerfile` 与 `duild-all.dockerfile` 内容一致，用哪个都行。

系统依赖走 Fedora 的 dnf：spdlog、GoogleTest、Asio、OpenSSL、liburing、gperftools 全部全局安装，CMake 配置阶段不需要联网下载依赖。vcpkg 也装了一份在 `/opt/vcpkg`，留给调用方自己用，镜像构建本身不靠它取项目依赖。实验用的 stdexec 按 `scripts/experimental_dependencies.lock.json` 锁定的版本取到 `/opt/faio-deps/src/stdexec`，再软链到 `/usr/local/include`。

GCC 16.1 和配套运行库来自 Fedora 上 SHA256 固定的 `16.1.1-2.fc44` RPM，装进 `/opt/gcc-16.1`，然后把 `/usr/bin/gcc`、`/usr/bin/g++` 指过去，`LD_LIBRARY_PATH` 里带上对应的 libstdc++。Clang 的处理要分两种：开发镜像和稳定特性构建镜像装完整的 clang 23，用 `--gcc-toolchain=/opt/gcc-16.1/usr` 让它吃 GCC 16.1 的标准库；全特性镜像和根 Dockerfile 不装 Clang 编译器，只从 clang-tools-extra 包里取出 clangd 和它需要的运行库。实验工具链的验证脚本会检查镜像里没有 clang、clang-tidy、LLD 和 LLDB，这两份镜像只保留 clangd 就是配合这个检查。

```sh
docker build -f docker/dev-image.dockerfile -t faio:dev .
docker build -f docker/duild-all.dockerfile -t faio:all .
docker build -f docker/build-main.dockerfile -t faio:main .
docker build -t faio:all .
```

最后一条用根目录 Dockerfile，产物和 `faio:all` 相同。构建过程中会编译全部 C++ 示例、单元测试、性能基准，以及 `benchmark/{coro,io,tcp}/tokio-benchmark` 三个锁定版本的 Tokio 对照程序，最后 `cmake --install` 装到 `/usr/local`。`BUILD_TYPE` 取 `Release` 或 `Debug`，默认 Release；`BUILD_JOBS` 控制 `--parallel` 的并发度，默认 4。产物按预设落在 `target/cmake-build-linux-{all,main}-<构建类型>/`。构建阶段不跑依赖宿主内核的 I/O 测试。

```sh
docker build -f docker/build-main.dockerfile --build-arg BUILD_TYPE=Debug --build-arg BUILD_JOBS=2 -t faio:main-debug .
docker run --rm --security-opt seccomp=unconfined faio:all ctest --preset linux-all-release
docker run --rm -it -v "$PWD:/workspace/faio" -w /workspace/faio faio:dev
```

io_uring 能不能用取决于宿主内核和容器安全配置，后者需要放开对应的系统调用，上面第二行命令为此加了 `--security-opt seccomp=unconfined`。默认 seccomp 下想走 epoll，配置时加 `-DFAIO_ENABLE_IO_URING=OFF`。`BASE_IMAGE` 可以换掉默认的 `fedora:45`，替代镜像得自带 Clang 23 和前面那批依赖；`KOJI_ADDRESS` 只在这一条构建里覆盖官方 RPM 源的 DNS 解析，HTTPS 证书仍然按域名校验。

## Dev Container

`.devcontainer/devcontainer.json` 复用 `dev-image.dockerfile`，CLion 会把 devcontainer 打成 `jb-devcontainer-<name>` 镜像，所以 `name` 必须是合法的 Docker 仓库名：只能有小写字母、数字、`.`、`_`、`-`，带上空格、`·`、`/`、`+` 会直接报 `invalid tag ... invalid reference format`。

开发镜像里 Bazel 和 xmake 都是装好的，容器开起来就能用。Bazel 的版本由 `.bazelversion` 锁定在 9.2.0，按架构下载官方发行包并校验 SHA256，`/usr/local/bin/bazel` 是一个跟随工作区根目录的 dispatch 包装脚本；xmake 是 3.0.9，从校验过的源码包编译安装。CMake、Bazel、xmake 三条路径共用同一套目录约定，产物都收在 `target/` 下：

| 构建系统 | 输出位置 |
|---|---|
| CMake | `target/cmake-build-<preset>/` |
| Bazel | `target/bazel/`，含产物与便捷链接 |
| xmake | `target/xmake/` |

devcontainer 把 `target/` 挂成独立的命名数据卷，构建产物留在卷里，不散落到宿主目录。Bazel 的落点由 `.bazelrc` 保证：`--output_user_root` 和 `--symlink_prefix` 都指向 `target/bazel/`，所以从仓库的任意子目录跑 `bazel`，输出仍然回到仓库根下的同一处。xmake 的 builddir 在 `xmake.lua` 里就设成了 `target/xmake`，devcontainer 再用 `XMAKE_CONFIGDIR` 把配置目录也放到那里。

镜像还把 SHA256 固定的 [clice](https://clice.io) 1.0.0 解压到 `/root/.local`，可执行文件是 `/root/.local/bin/clice`，`lib/clang/23` 资源目录一并保留，并已加进 `PATH`。`postCreateCommand` 先跑一次 `cmake --preset linux-all-debug`，再把 `target/compile_commands.json` 链到 `target/cmake-build-linux-all-debug/compile_commands.json`。

clice 的行为由仓库根的 `clice.toml` 决定：编译数据库显式指向 `target`，免得 clice 自己挑中 CLion 生成的 `cmake-build-*` 目录；索引缓存放在本地盘 `/tmp/faio-clice-cache`，因为 dev container 的 `/workspace` 是 macOS 共享挂载，clice 的索引库需要本地文件系统语义。VS Code 通过 `clice-io.clice` 扩展启用它，`.devcontainer/devcontainer.json` 里的 `clice.executable` 指向上面的二进制。Zed 得从扩展市场自己装 clice 扩展，`.zed/settings.json` 已经把 `lsp.clice.binary.path` 指向同一路径；在宿主机上用 Zed 时，这个路径要改成本机的 clice，或者删掉这一项，让扩展使用自带或自动下载的版本。

`clice.toml` 里没有额外的编译选项。experimental 目标的 `-std=c++26` 是 CMake 写进编译数据库的，clice 直接照用。GCC 16.1 的反射扩展（`-freflection` 和 `<meta>`）不在 clice 内置 clang 23 支持的方言里，反射源码在 clice 下仍会出现诊断，但实际编译由 GCC 完成，不影响构建。clangd 照常安装和配置，只是默认关闭：VS Code 里是 `clangd.enable: false`，Zed 里是 `language_servers: ["clice", "!clangd"]`。

镜像另外装了 `openssl3-libs`。Fedora 45 只带 OpenSSL 4，而 CLion 的 .NET（Rider/radler）索引后端仍然链 `libssl.so.3`，缺了它后端会以 `No usable version of libssl was found` 崩溃，退出码 134。

## CMake 预设

预设名按 `{平台}-{特性}-{构建类型}` 拼，configure、build、test 三段同名。Linux 是 `linux-{all,main}-{release,debug}`，macOS 是 `mac-{all,main}-{release,debug}`，Windows 是 `windows-{clang,mingw,msvc}-{all,main}-{release,debug}`。

`all` 表示明确要全部实验组件。眼下实验实现只在 Linux、GCC 16.1 和匹配的 libstdc++ 上成立，所以 macOS 和 Windows 上传 `all` 会在配置阶段就报出平台限制，这两个平台的稳定构建用 `main`。macOS 的 `main` 需要把 Homebrew LLVM 23 的 `bin` 加进 `PATH` 并指定 Homebrew 依赖前缀。Windows 的 `main` 分别走 clang-cl、MinGW-w64 和 MSVC，依赖由各编译器独立构建。

CLion 里的工具链配置步骤见 [CLion.md](CLion.md)。

工具链来源：[Fedora GCC](https://packages.fedoraproject.org/pkgs/gcc/gcc/)、[Fedora Clang 23](https://packages.fedoraproject.org/pkgs/llvm/clang/fedora-45.html)、[clangd 包](https://packages.fedoraproject.org/pkgs/llvm/clang-tools-extra/)、[vcpkg 固定发行版](https://github.com/microsoft/vcpkg/releases/tag/2026.10.16)。

## Linux 分配器

`FAIO_ALLOCATOR=AUTO|SYSTEM|TCMALLOC` 决定进程用哪个分配器。`AUTO` 在 Linux 上检测到 tcmalloc 且没开检测器时用 tcmalloc，其他平台和检测器构建回落到系统分配器。显式写 `TCMALLOC` 要求 Linux 和可用的 gperftools，不能和检测器混用。

链接 tcmalloc 等于换掉整个进程的 malloc/new，协程帧缓存和大帧池的配置照旧生效，`FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW` 默认 OFF。安装包被别的项目消费时，分配器按同一个选项重新解析一遍，选 `SYSTEM` 就不需要 gperftools。检测覆盖的是公共 CMake flags 和配置专用 flags；包导入的时候没法预知你之后用 `target_compile_options` 加的检测器，所以手动给最终目标加检测器时，消费项目要显式写 `-DFAIO_ALLOCATOR=SYSTEM`，或者用 `FAIO_SANITIZERS` 声明检测器配置。

英文版在 [README_EN.md](README_EN.md)，两篇内容一致。
