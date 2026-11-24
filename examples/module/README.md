# 使用 faio 具名模块

[`quickstart.cpp`](quickstart.cpp) 只写 `import faio;`，创建两个异步任务，并发等待它们得到 42。程序通过 `faio::block_on` 在调用线程上驱动任务完成，成功时返回退出码 0。使用协程无需额外包含 `<coroutine>` 或导入 `std`。

示例的 [`CMakeLists.txt`](CMakeLists.txt) 可以直接作为下游项目的起点。按功能导入的模块和 CMake、Bazel、xmake 下游配置见[模块移植说明](../../cxx-module/README.md)。

## 从源码接入

需要 CMake 3.28+、Ninja 1.11+、spdlog 和支持 C++23 具名模块的编译器。Clang 的 `clang++` 与 `clang-scan-deps` 必须来自同一份 LLVM 安装，macOS 使用 Homebrew LLVM。

在 faio 仓库根目录执行。示例复用 `cxx-module/` 的导入入口和 `include/faio/` 的共享实现，默认关闭 io_uring：

```sh
cmake -S examples/module -B target/cmake-build-module-example -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build target/cmake-build-module-example
./target/cmake-build-module-example/quickstart_module
```

把示例复制到自己的项目后，用 `-DFAIO_SOURCE_DIR=/path/to/faio` 指定 faio 仓库位置。目标链接 `faio::module`，`import` 依赖由 CMake 扫描。构建时启用 TLS 或异步进程后，统一目标与 `import faio;` 会包含相应接口。

## 从安装包接入

先构建并安装模块包，这里把前缀放在仓库的 `target/` 下：

```sh
cmake -S cxx-module -B target/cmake-build-module-provider -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release \
  -DFAIO_ENABLE_IO_URING=OFF
cmake --build target/cmake-build-module-provider
cmake --install target/cmake-build-module-provider --prefix "$PWD/target/faio-module-prefix"
```

消费方必须使用兼容的编译器、标准库和构建配置。加上 `FAIO_MODULE_EXAMPLE_USE_PACKAGE=ON` 后，示例改用 `find_package(faioModules CONFIG REQUIRED)` 获取同一个链接目标：

```sh
cmake -S examples/module -B target/cmake-build-module-example-installed -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release \
  -DFAIO_MODULE_EXAMPLE_USE_PACKAGE=ON -DCMAKE_PREFIX_PATH="$PWD/target/faio-module-prefix"
cmake --build target/cmake-build-module-example-installed
./target/cmake-build-module-example-installed/quickstart_module
```

两种方式都通过退出码检查结果，不输出文本。

## 用 Bazel 或 xmake 运行

Bazel 沿用支持具名模块的 Clang 工具链：

```sh
export CC="$(command -v clang)"
bazel run --config=faio_modules //examples:quickstart_module
```

xmake 在 macOS 上使用 Homebrew LLVM，Linux 开发容器里把编译器路径换成 `/usr/sbin/clang++`：

```sh
faio_cxx="$(brew --prefix llvm)/bin/clang++"
xmake f -c -y -m release --toolchain=clang \
  --cxx="$faio_cxx" --ld="$faio_cxx" --faio_modules=y
xmake run quickstart_module
```

两个构建入口的目标名和下游接入方式见[模块移植说明](../../cxx-module/README.md)。
