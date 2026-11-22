# 使用 faio 具名模块

[`quickstart.cpp`](quickstart.cpp) 用 `import faio;` 创建一个异步任务，等待 10 毫秒后返回 `42`，由 `faio::block_on` 在调用线程上驱动到完成。整个程序只依赖核心模块和标准库。

示例的 [`CMakeLists.txt`](CMakeLists.txt) 可以直接作为下游项目的起点。CMake、Bazel 和 xmake 的下游配置汇总在[模块移植说明](../../cxx-module/README.md)。

## 工具链

需要 CMake 3.28+、Ninja 1.11+、支持 C++23 的编译器，以及同一份 LLVM 安装里的 `clang-scan-deps`。核心模块链接 spdlog。macOS 把 Homebrew LLVM 的 `bin` 加入 `PATH`；faio 的 Linux 开发容器里，下文的 `clang++` 要换成 `clang++-23`。

## 从源码接入

在 faio 仓库根目录执行。示例复用 `cxx-module/` 的导入入口和 `include/faio/` 的共享实现，默认选择 epoll：

```sh
cmake -S examples/module -B target/cmake-build-module-example -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build target/cmake-build-module-example
./target/cmake-build-module-example/quickstart_module
```

把示例复制到自己的项目后，用 `-DFAIO_SOURCE_DIR=/path/to/faio` 指定 faio 仓库位置。目标链接 `faio::module`，`import` 依赖由 CMake 扫描。

## 从安装包接入

先构建并安装模块包，这里把前缀放在仓库的 `target/` 下：

```sh
cmake -S cxx-module -B target/cmake-build-module-provider -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release \
  -DFAIO_ENABLE_IO_URING=OFF
cmake --build target/cmake-build-module-provider
cmake --install target/cmake-build-module-provider --prefix "$PWD/target/faio-module-prefix"
```

消费方必须使用相同的编译器、标准库和构建配置。加上 `FAIO_MODULE_EXAMPLE_USE_PACKAGE=ON` 后，示例改用 `find_package(faioModules CONFIG REQUIRED)` 获取同一个链接目标：

```sh
cmake -S examples/module -B target/cmake-build-module-example-installed -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release \
  -DFAIO_MODULE_EXAMPLE_USE_PACKAGE=ON -DCMAKE_PREFIX_PATH="$PWD/target/faio-module-prefix"
cmake --build target/cmake-build-module-example-installed
./target/cmake-build-module-example-installed/quickstart_module
```

两种方式都输出 `The answer is 42`。

## 用 Bazel 或 xmake 运行

在仓库根目录用 Bazel 构建并运行同一个程序：

```sh
bazel run --config=faio_modules //examples:quickstart_module
```

xmake 需要先打开模块特性，再运行同名目标：

```sh
xmake f -c -y -m release --toolchain=clang \
  --cxx="$(command -v clang++)" --ld="$(command -v clang++)" --faio_modules=y
xmake run quickstart_module
```

两个构建入口的目标名和下游接入方式见[模块移植说明](../../cxx-module/README.md)。
