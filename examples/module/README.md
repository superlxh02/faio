# 使用 faio 具名模块

[`quickstart.cpp`](quickstart.cpp) 用 `import std;` 和 `import faio;` 创建一个异步任务，等待 10 毫秒后输出 `Hello, world!`，由 `faio::block_on` 在调用线程上驱动到完成。整个程序只依赖核心模块和标准库模块。

示例的 [`CMakeLists.txt`](CMakeLists.txt) 可以直接作为下游项目的起点。CMake、Bazel 和 xmake 的下游配置汇总在[模块移植说明](../../cxx-module/README.md)。

## 工具链

需要 CMake 3.28+、Ninja 1.11+ 和 GCC 16.1 及以上。核心模块链接 spdlog。

示例默认 `import std;`，由 GCC 的 `libstdc++.modules.json` 提供 `std` 接口单元：CMake 通过 `cmake/FaioImportStd.cmake` 打开实验开关并设置 `CXX_MODULE_STD`，Bazel 通过 `//bazel:std_module.bzl` 生成 `@gcc_std_module//:std`，xmake 通过 `build.c++.modules.std` 策略扫描同一份清单。没有这个模块的编译器无法构建本示例；仓库根目录的构建会跳过 `quickstart_module`，其他示例不受影响。

## 从源码接入

在 faio 仓库根目录执行。示例复用 `cxx-module/` 的导入入口和 `include/faio/` 的共享实现，默认选择 epoll：

```sh
cmake -S examples/module -B target/cmake-build-module-example -G Ninja \
  -DCMAKE_CXX_COMPILER=g++-16 -DCMAKE_BUILD_TYPE=Release
cmake --build target/cmake-build-module-example
./target/cmake-build-module-example/quickstart_module
```

把示例复制到自己的项目后，用 `-DFAIO_SOURCE_DIR=/path/to/faio` 指定 faio 仓库位置。目标链接 `faio::module`，`import` 依赖由 CMake 扫描。

## 从安装包接入

先构建并安装模块包，这里把前缀放在仓库的 `target/` 下：

```sh
cmake -S cxx-module -B target/cmake-build-module-provider -G Ninja \
  -DCMAKE_CXX_COMPILER=g++-16 -DCMAKE_BUILD_TYPE=Release \
  -DFAIO_ENABLE_IO_URING=OFF
cmake --build target/cmake-build-module-provider
cmake --install target/cmake-build-module-provider --prefix "$PWD/target/faio-module-prefix"
```

消费方必须使用相同的编译器、标准库和构建配置。加上 `FAIO_MODULE_EXAMPLE_USE_PACKAGE=ON` 后，示例改用 `find_package(faioModules CONFIG REQUIRED)` 获取同一个链接目标，`import std;` 的开关则在 `CMAKE_PREFIX_PATH` 下安装的 `FaioImportStd.cmake` 里找到：

```sh
cmake -S examples/module -B target/cmake-build-module-example-installed -G Ninja \
  -DCMAKE_CXX_COMPILER=g++-16 -DCMAKE_BUILD_TYPE=Release \
  -DFAIO_MODULE_EXAMPLE_USE_PACKAGE=ON -DCMAKE_PREFIX_PATH="$PWD/target/faio-module-prefix"
cmake --build target/cmake-build-module-example-installed
./target/cmake-build-module-example-installed/quickstart_module
```

两种方式都会输出 `Hello, world!`。

## 用 Bazel 或 xmake 运行

Bazel 把 `CC` 指向 GCC 16.1 及以上，用 `faio_std_modules` 配置构建并运行同一个程序：

```sh
export CC="$(command -v g++-16)"
bazel run --config=faio_std_modules //examples:quickstart_module
```

xmake 用 GCC 工具链并显式给出三个 driver：

```sh
xmake f -c -y -m release --toolchain=gcc \
  --cc="$(command -v gcc-16)" --cxx="$(command -v g++-16)" --ld="$(command -v g++-16)" \
  --faio_modules=y
xmake run quickstart_module
```

两个构建入口的目标名和下游接入方式见[模块移植说明](../../cxx-module/README.md)。
