# CLion Docker工具链

```sh
docker build -f docker/dev-image.dockerfile -t faio:dev .
```

在CLion的工具链设置中添加Docker环境并选择对应镜像。

| 配置 | 路径 |
|---|---|
| C编译器 | `/usr/local/bin/faio-cc` |
| C++编译器 | `/usr/local/bin/faio-cxx` |
| CMake | `/usr/bin/cmake` |
| Ninja | `/usr/bin/ninja` |
| 调试器 | `/usr/bin/gdb` |
| clangd | `/usr/local/bin/clangd` |

同一`faio:dev`镜像提供GCC 16.1、Clang 23、clangd和锁定stdexec。主线选择`linux-main-debug`或`linux-main-release`；实验开发选择`linux-all-debug`或`linux-all-release`。默认`faio-cc/faio-cxx`指向Clang 23，all预设显式使用GCC 16.1。GCC 16.1路径为`/opt/gcc-16.1/usr/bin/g++`，Clang 23路径为`/usr/local/bin/clang++-23`。CMake按目标设置C++23/26及反射选项，项目不需要全局C++ flags或额外依赖挂载。

项目挂载到`/workspace/faio`。系统依赖及锁定stdexec位于挂载目录之外。镜像更新后重新检测工具链，执行Reset Cache and Reload Project。io_uring运行及调试需要宿主内核和容器安全配置支持；epoll使用`-DFAIO_ENABLE_IO_URING=OFF`。
