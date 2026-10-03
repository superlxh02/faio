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
