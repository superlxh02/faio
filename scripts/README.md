# 构建与验证脚本

日常配置、编译和单元测试走根目录的 CMake 预设，不需要动这里的脚本。这些脚本处理另外几件事：准备不随仓库分发的依赖，生成发布源码包，以及在独立工程里验证安装后的 faio 能否被下游正常消费。

## 下游消费验证

四个 `verify_*` 脚本都会新建临时下游工程，不复用仓库自己的构建缓存，临时目录留在 `target/verification/<tool>/`。它们检查的是安装包和源码消费路径，和日常预设构建分开跑。

### verify_build_organization.py

这是统一入口，按构建工具和后端调度下面三个脚本。每个组合是一个独立的 cell，某个 cell 失败不会中断其余 cell，全部跑完后再返回非零状态。

```sh
python3 scripts/verify_build_organization.py --backend kqueue
python3 scripts/verify_build_organization.py --docker-image faio:dev --tool cmake --backend all --compiler /usr/bin/clang++
```

`--tool` 可重复，取 `cmake`、`bazel`、`xmake`，默认三个都跑。`--backend` 取 `all`、`epoll`、`io_uring`、`kqueue`；默认的 `all` 在 Linux 上跑 epoll 和 io_uring 两个后端，在 macOS 上只跑 kqueue。在 Linux 容器里验证时传 `--docker-image`，镜像要自带所选工具、依赖和 registry 访问或缓存。io_uring 后端会加上 `seccomp=unconfined`，因为默认的 Docker seccomp 配置会拒绝 `io_uring_setup`。

工具链相关参数：`--compiler` 用于 CMake 和 xmake（在 Docker 中作用于容器内），`--bazel-cc` 指定 Bazel 本地工具链的 clang，`--bazel` 指定 Bazel 或 Bazelisk 可执行文件。`--xmake-offline` 打开 xmake 的私有网络模式。只想知道会执行哪些命令时加 `--dry-run`。

### verify_cmake_consumers.py

分别构建并安装头文件包和模块包，把安装前缀搬到新位置，再用独立的下游工程消费。覆盖 quickstart、foundation、核心、TLS、进程以及 TLS 与进程组合，同时检查头文件包里没有混入模块接口、模块包里带有共享头。参数只有两个：

```sh
python3 scripts/verify_cmake_consumers.py --compiler clang++
python3 scripts/verify_cmake_consumers.py --compiler clang++ --io-uring
```

`--compiler` 传给 `CMAKE_CXX_COMPILER`。`--io-uring` 在 Linux 上把后端从默认的 epoll 换成 io_uring。

### verify_bazel_consumers.py

在独立的 Bzlmod 根工程里，通过 `local_path_override` 接入 faio 源码，验证头文件形式和模块形式的消费。组件矩阵覆盖核心、TLS、进程和两者组合；选择模块形式时还会检查增量构建，确认改导出的声明或改消费端编译单元时重建范围符合预期。

```sh
python3 scripts/verify_bazel_consumers.py --form both --suite all --jobs 4
```

`--form` 取 `headers`、`modules`、`both`，默认 `both`。`--suite` 取 `all`、`components`、`incremental`，默认 `all`；`incremental` 只能配合模块形式。`--io-uring` 取 `auto`、`on`、`off`，Linux 上默认 `off`，其他平台默认 `auto`。`--jobs` 默认 4，`--bazel` 默认 `bazel`。

### verify_xmake_consumers.py

在隔离的 xmake 工程里验证从源码消费头文件形式和模块形式，覆盖核心、TLS、进程和组合场景；随后安装模块包、搬到新位置，再由另一个独立工程消费。

```sh
python3 scripts/verify_xmake_consumers.py --compiler "$(command -v clang++)" --offline
```

`--compiler` 必填，是 clang++ 的路径。`--io-uring` 选择 Linux 后端；`--offline` 把 xmake 切到私有网络模式，适合依赖已经缓存好的机器。

## 依赖引导与打包

其余文件分成实验依赖、Windows 环境和源码打包三组：

| 文件 | 用途 |
|---|---|
| `bootstrap_experimental_deps.py` | 准备日常构建的 stdexec checkout，或为独立实验副本获取、离线重建及校验外部对照源码 |
| `experimental_dependencies.lock.json` | 锁定 stdexec、Asio standalone、Flare 与 Flare native fiber 的 Bazel 源码归档 |
| `bootstrap_windows.ps1` | 下载并校验 Windows 依赖源码与 MinGW 工具链 |
| `windows_environment.ps1` | 将 MSVC、Clang 或 MinGW 环境导入当前 PowerShell 进程 |
| `windows-dependencies.json`、`windows-mingw-packages.json` | Windows 依赖和工具链软件包清单 |
| `package_source.py` | 生成发布源码包、文件清单和 SHA256 摘要 |

### 实验依赖

不带参数时，脚本在 `../faio-deps/src/stdexec` 获取或校验 CMake 构建使用的 stdexec Git checkout；缓存位置可以用 `--dependency-root` 改。

```sh
python3 scripts/bootstrap_experimental_deps.py
```

准备独立的实验副本时，指定实验根目录和可复制的共享归档缓存：

```sh
python3 scripts/bootstrap_experimental_deps.py --experiment-root "$FAIO_EXPERIMENT" \
  --archive-dir "$FAIO_ARCHIVES" --with-flare-bazel
```

板端测试用 rsync 同步同一份已验证的 `third_party/` 源码、归档缓存、同版脚本及锁文件，以及在本机 Linux 构建好的可执行文件。板端只验证并运行已有产物：

```sh
python3 "$FAIO_EXPERIMENT/scripts/bootstrap_experimental_deps.py" \
  --experiment-root "$FAIO_EXPERIMENT" --archive-dir "$FAIO_ARCHIVES" \
  --offline --verify-only --with-flare-bazel
```

要从缓存重建本机实验副本，给上面的准备命令加 `--offline`。缓存缺失、归档摘要不符或已有源码被改动都会报错。源码分别放在 `third_party/stdexec`、`third_party/asio-standalone` 和 `third_party/flare`。`third_party/manifest.json` 记录仓库、归档 URL、tag、commit、SHA256、源码树摘要和入口头文件。Flare 的固定提交没有上游标签，因此 `tag` 记为 `null`。

Asio 对照使用 standalone 头文件，例如 `#include <asio/awaitable.hpp>` 和 `asio::co_spawn`，构建参数为 `-DASIO_STANDALONE -I"$FAIO_EXPERIMENT/third_party/asio-standalone/include"`。stdexec 对照把 `third_party/stdexec/include` 作为头文件根目录；faio 的 `FAIO_STDEXEC_SOURCE_DIR` 仍然接收前述 Git checkout 或锁定的 vcpkg 包。Flare 对照先把锁定源码复制到独立工作区，保留一份可复核的 `third_party/`，再把原生 fiber 适配器加入工作区。Bazel 使用 7.5.0，并通过 `--noenable_bzlmod --repository_disable_download --distdir="$FAIO_ARCHIVES"` 把依赖限制在锁定的源码归档内。这个 distdir 覆盖 Flare native fiber 对照和探针的构建依赖，操作系统工具链与 Bazel 可执行文件由执行环境提供。

### 源码包

```sh
python3 scripts/package_source.py --output target/faio-source.tar.gz
```

生成的归档包含代码、示例、基准、单元测试和用户文档；开发过程资料、本地缓存和构建产物不会打进去。

### Windows

`bootstrap_windows.ps1` 负责下载并校验 Windows 依赖源码和 MinGW 工具链，`windows_environment.ps1` 把 MSVC、Clang 或 MinGW 的环境变量导入当前 PowerShell 进程。依赖版本和工具链软件包分别记在 `windows-dependencies.json` 与 `windows-mingw-packages.json`。
