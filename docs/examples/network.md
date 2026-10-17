# TCP 与 UDP 服务

这组例程是常驻服务。运行服务后，在另一个终端发送数据；验证结束使用 `Ctrl+C` 终止。两个 TCP 版本使用同一端口，请分别运行。

## TCP 回显服务

```sh
cmake --build build/local --target tcp_echo_server
./build/local/examples/tcp_echo_server
```

在第二个终端连接并输入文字，观察收到的原样回显：

```sh
nc 127.0.0.1 8080
```

监听器接受连接后，把移动独占的 `TcpStream` 交给连接协程。连接循环使用非空缓冲读取，检查错误和 EOF，再通过 `write_all()` 处理可能的短写。

### 完整源码：TCP

```cpp
{{#include ../../examples/tcp_echo_server.cpp}}
```

## UDP 回显服务

```sh
cmake --build build/local --target udp_echo_server
./build/local/examples/udp_echo_server
```

另一个终端：

```sh
nc -u 127.0.0.1 9090
```

每个 UDP 数据报单独处理并回给来源地址。零长度数据报是有效消息，不能使用 TCP 的 EOF 规则。

### 完整源码：UDP

```cpp
{{#include ../../examples/udp_echo_server.cpp}}
```

## 单线程版本

```sh
cmake --build build/local --target tcp_echo_server_single_thread
./build/local/examples/tcp_echo_server_single_thread
```

客户端仍连接 `127.0.0.1:8080`。重点比较 main 中的 `current_thread` 配置；监听、读写与连接生命周期仍完整地写在本文件中。

### 完整源码：单线程 TCP

```cpp
{{#include ../../examples/tcp_echo_server_single_thread.cpp}}
```

[API：网络](../api/network.md) · [运行时](../api/runtime.md) · [网络模块设计](../网络IO.md)
