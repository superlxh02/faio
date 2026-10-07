# Docker 开发环境

**简体中文** | [English](README_EN.md)

本目录用于 faio 的开发、调试和性能测试。以下命令均在仓库根目录执行，构建上下文使用 `.`。

| 文件                                   | 用途                                                                |
|--------------------------------------|-------------------------------------------------------------------|
| [env.dockerfile](env.dockerfile)     | 安装 GCC、CMake、调试器、测试及 benchmark 依赖，并缓存两个 Tokio 项目的锁定依赖；不编译 faio 源码 |
| [build.dockerfile](build.dockerfile) | 基于环境镜像复制源码，编译 C++ 示例、测试、benchmark 和两个 Rust benchmark              |

创建环境镜像，并挂载工作目录开发：

```bash
docker build -f docker/env.dockerfile -t faio:dev .
docker run --rm -it --security-opt seccomp=unconfined \
  -v "$PWD:/workspace/faio" -w /workspace/faio faio:dev

# 容器内
cmake -S . -B build/docker-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/docker-debug -j4
ctest --test-dir build/docker-debug --output-on-failure
```

生成包含构建产物的开发镜像，并运行测试：

```bash
docker build -f docker/build.dockerfile -t faio:build .
docker run --rm --security-opt seccomp=unconfined faio:build \
  ctest --test-dir build --output-on-failure
```

构建镜像支持 `DEV_IMAGE`、`CMAKE_BUILD_TYPE` 和 `BUILD_JOBS` 参数，例如 `--build-arg CMAKE_BUILD_TYPE=Debug`。依赖变化时重新构建环境镜像；源码变化时只需重建构建镜像，Rust 编译使用缓存的依赖离线进行。

`seccomp=unconfined` 允许测试和示例调用 io_uring；Docker 构建阶段只编译，不运行依赖 io_uring 的程序。实际运行依赖宿主内核支持对应操作。

## GCC 16.1 实验环境与 P0 验证

[experimental_env.dockerfile](experimental_env.dockerfile) 参考 Fedora Rawhide 工具链环境，
但不会把 Rawhide 当前的 GCC 版本当作实验基线。它从 Fedora Koji 固定获取
`16.1.1-2.fc44` 的 GCC、libstdc++、libgcc 与 sanitizer RPM，按
[gcc16.1.sha256](gcc16.1.sha256) 校验并提取至 `/opt/gcc-16.1`。
系统工具仍可使用 Rawhide 自身的库；faio 使用配套 16.1 编译器和运行库。
环境构建会真实编译运行 `<meta>`、注解、extract 和函数 splice，并分别检查
ASan+UBSan 与 TSan 的编译链接能力。CMake 必须至少为 4.1。
镜像已包含固定 stdexec、spdlog、GoogleTest、liburing、Asio、CMake、Ninja 和 gdb；
stdexec 保存在 `/opt/faio-deps/src/stdexec`，不会被 IDE 的项目挂载覆盖。

```bash
docker build -f docker/experimental_env.dockerfile -t faio:experimental-dev .
docker run --rm -it --security-opt seccomp=unconfined \
  -v "$PWD:/workspace/faio" \
  -w /workspace/faio faio:experimental-dev

# 容器内：镜像 ENV 提供实验开关与依赖路径，configure 不访问网络。
cmake -S . -B build/docker-experimental -DCMAKE_BUILD_TYPE=Debug
cmake --build build/docker-experimental -j2 \
  --target faio_experimental_execution_current_thread faio_experimental_async_main_execution
```

镜像 ENV 默认 `FAIO_ENABLE_EXPERIMENTAL=ON`、`FAIO_EXPERIMENTAL_REFLECTION=ON`、
`FAIO_EXPERIMENTAL_EXECUTION=ON`、`FAIO_STDEXEC_SOURCE_DIR=/opt/faio-deps/src/stdexec`。
入口默认 `NORMAL`；ASYNC 源文件包含入口头并使用一次性 `main(入口函数名)` 标记宏，
`faio_configure_entry(... MODE ASYNC)` 仅配置目标编译属性，不生成 main。sanitizer 与原始失败证据不会自动开启。显式 `-D`、preset 和已有 CMake cache
优先于 ENV；基础构建可传 `-DFAIO_ENABLE_EXPERIMENTAL=OFF`。
ENV 启用组件后，使用 execution 头的目标仍须链接 `faio::experimental_execution`，
以获得对应能力定义和 stdexec include 路径；`faio_experimental_async_main` 示例已处理此链接。

CLion Docker toolchain 选择 `faio:experimental-dev`，C/C++ 编译器可直接使用
`/usr/bin/gcc`、`/usr/bin/g++`，两者指向配套 GCC 16.1；调试器为 `/usr/bin/gdb`，
CMake/Ninja 为 `/usr/bin/cmake`、`/usr/bin/ninja`。Rawhide 原编译器保留为
`/usr/bin/gcc.rawhide`、`/usr/bin/g++.rawhide`。
更新镜像后重新创建或检测 Docker toolchain 容器，并在 CLion 执行 **Reset Cache and Reload Project**，
以免旧容器或旧的 `FAIO_ENABLE_EXPERIMENTAL=OFF` cache 继续生效。无需 entrypoint 初始化、
额外依赖挂载、`CPATH` 或全局 C++ flags。裸 `#include <exec/task.hpp>` 也可直接找到；
实验目标仍由 CMake 传递正确标准与能力定义。固定依赖路径已配置 Git safe.directory，支持非 root UID 读取。

[experimental_build.dockerfile](experimental_build.dockerfile) 构建两个独立目录：
`build/experimental-base` 保持 C++23 和实验开关 OFF；`build/experimental-enabled`
启用 reflection/execution，构建基础与实验示例、测试。配置、构建及实验 CTest
在 `RUN --network=none` 中执行，使用开发镜像内已有的固定依赖。

```bash
docker build -f docker/experimental_build.dockerfile -t faio:experimental-build .
docker run --rm --security-opt seccomp=unconfined faio:experimental-build \
  ctest --test-dir build/experimental-enabled --output-on-failure
```

构建参数包括 `DEV_IMAGE`、`CMAKE_BUILD_TYPE`、`BUILD_JOBS`、`FAIO_ENABLE_IO_URING`
（默认 OFF）、`FAIO_SANITIZERS`、`FAIO_RUN_EXPERIMENTAL_TESTS`（默认 ON）和
`FAIO_INCLUDE_ORIGINAL_P0_EVIDENCE`（默认 OFF）。ASan+UBSan 使用
`--build-arg FAIO_SANITIZERS=address,undefined`；TSan 使用独立镜像和 `thread`。

固定 stdexec 对裸 `std::stop_token` 要求 `callback_type` 的两个原始失败仍保留。
正式 P0 使用用户批准的最小 stop-token wrapper，真实 GCC 16.1 已通过三个正向探针。
原始失败不是成功验收项，可在独立 P0 配置中设置
`FAIO_P0_ORIGINAL_STOP_TOKEN_EVIDENCE=ON` 复验；CTest 将诚实返回失败。

2026-10-06 已在 aarch64 容器实测 GCC `16.1.1 20260515 (Red Hat 16.1.1-2)`、
libstdc++ release `16` / header date `20260515` / `libstdc++.so.6.0.35`，
CMake `4.3.0`、Ninja `1.13.2`。反射编译运行与两个独立 sanitizer 配置的链接检查通过。
x86_64 的 RPM 也已获取并校验，尚未在 x86_64 运行上述检查。
运行记录及未执行项目见 [实验构建指南](../docs/experimental/build.md)。
本轮只验证开发镜像、离线默认配置、有限示例及调试器；没有运行完整测试或完成完整构建镜像验收。

环境镜像可通过 `BASE_IMAGE` 复用现有 Fedora 镜像。当前机器的 DNS 代理曾导致 Koji
HTTPS 连接中断；`KOJI_ADDRESS` 可为一次构建指定该官方域名的真实 IPv4 地址，
仍验证 HTTPS 域名和证书。例如本次复验命令使用：

```bash
docker build -f docker/experimental_env.dockerfile -t faio:experimental-dev \
  --build-arg BASE_IMAGE=modern-cpp-rawhide:1.0 \
  --build-arg KOJI_ADDRESS=38.145.32.21 .
```

普通环境不需要此参数；该地址不是固定供应源，使用前应核对官方 DNS。

也可用 BuildKit 的 named context 导入此前下载的 RPM 缓存，仍会验证同一 SHA256 和 NVR。
缓存目录的结构须为 `aarch64/*.rpm` 或 `x86_64/*.rpm`；本次实际环境镜像已通过此路径构建：

```bash
docker build -f docker/experimental_env.dockerfile -t faio:experimental-dev \
  --build-arg BASE_IMAGE=modern-cpp-rawhide:1.0 \
  --build-context gcc-rpm-cache=/tmp/faio-gcc16.1-rpms \
  --build-arg GCC_RPM_SOURCE=gcc-rpm-cache .
```

环境构建为验证过的 RPM 保留独立 BuildKit 缓存，后续构建可直接复用。
