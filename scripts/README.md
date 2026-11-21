# 构建辅助脚本

日常配置、构建和单元测试使用根目录的 CMake 预设。

| 文件 | 用途 |
|---|---|
| `bootstrap_experimental_deps.py` | 准备日常构建的 stdexec checkout，或为独立实验副本获取、离线重建及校验外部对照源码 |
| `experimental_dependencies.lock.json` | 锁定 stdexec、Asio standalone、Flare 与 Flare native fiber 的 Bazel 源码归档 |
| `bootstrap_windows.ps1` | 下载并校验 Windows 依赖源码与 MinGW 工具链 |
| `windows_environment.ps1` | 将 MSVC、Clang 或 MinGW 环境导入当前 PowerShell 进程 |
| `windows-dependencies.json`、`windows-mingw-packages.json` | Windows 依赖和工具链软件包清单 |
| `package_source.py` | 生成发布源码包、文件清单和 SHA256 摘要 |

```sh
python3 scripts/bootstrap_experimental_deps.py
python3 scripts/package_source.py --output build/faio-source.tar.gz
```

源码包包含代码、示例、基准、单元测试和用户文档；开发过程资料、本地缓存及构建产物不进入归档。

## 外部协程对照依赖

在本机用 Python 3.10+ 准备已有实验副本。`--archive-dir` 指向可复制的共享归档缓存。

```sh
python3 scripts/bootstrap_experimental_deps.py --experiment-root "$FAIO_EXPERIMENT" \
  --archive-dir "$FAIO_ARCHIVES" --with-flare-bazel
```

正式板测使用 rsync 同步同一份已验证的 `third_party/` 源码、归档缓存、同版脚本及锁和本机 Linux 构建的可执行文件。板端将 `FAIO_EXPERIMENT` 和 `FAIO_ARCHIVES` 指向同步位置，只验证并运行产物。

```sh
python3 "$FAIO_EXPERIMENT/scripts/bootstrap_experimental_deps.py" \
  --experiment-root "$FAIO_EXPERIMENT" --archive-dir "$FAIO_ARCHIVES" \
  --offline --verify-only --with-flare-bazel
```

要从缓存重建本机实验副本，给第一条命令加 `--offline`。缓存缺失、归档摘要不符或已有源码改动都会报错。源码路径为 `third_party/stdexec`、`third_party/asio-standalone` 和 `third_party/flare`。`third_party/manifest.json` 记录仓库、归档 URL、tag、commit、SHA256、源码树摘要以及入口头文件。Flare 的固定提交无上游标签，`tag` 记录为 `null`。日常执行不带 `--experiment-root` 时仍在 `../faio-deps/src/stdexec` 获取或校验 CMake 使用的 Git checkout，可用 `--dependency-root` 指定该缓存根路径。

Asio 适配器使用 `#include <asio/awaitable.hpp>` 等 standalone 头文件以及 `asio::co_spawn`，构建时传 `-DASIO_STANDALONE -I"$FAIO_EXPERIMENT/third_party/asio-standalone/include"`。stdexec 对照以 `third_party/stdexec/include` 为头文件根目录，faio 的 `FAIO_STDEXEC_SOURCE_DIR` 继续接收上文的 Git checkout 或锁定的 vcpkg 打包。Flare 构建应将锁定源码复制到独立工作区，以保留可复核的 `third_party/`，再向工作区加入原生 fiber 适配器。使用已安装的 Bazel 7.5.0，并通过 `--noenable_bzlmod --repository_disable_download --distdir="$FAIO_ARCHIVES"` 把依赖限制在锁定的工作区源码归档。该 distdir 覆盖 Flare native fiber 对照及探针的构建依赖；操作系统工具链和 Bazel 可执行文件由执行环境提供。
