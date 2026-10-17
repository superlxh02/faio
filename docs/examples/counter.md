# 共享计数器应用

`tcp_counter_server` 把监听、连接处理、文本协议、并发共享状态与超时处理放在一个完整程序中，是从回显服务过渡到应用服务的例子。

## 自动验证

```sh
cmake --build build/local --target tcp_counter_server
./build/local/examples/tcp_counter_server --self-test
```

`--self-test` 启动服务与客户端交互，完成检查后自行退出，适合先确认构建环境。

## 启动常驻服务

```sh
./build/local/examples/tcp_counter_server
```

监听 `127.0.0.1:8081`。完整命令协议和交互示例见 [示例 README](../../examples/README.md)，实现中的解析函数给出各命令、参数和失败响应。

## 阅读重点

- TCP 字节流如何按行拆分请求，以及处理一次读取中的多个命令。
- 多个连接如何访问共享计数状态，临界区如何界定。
- 如何处理超时、错误响应、断连与资源生命周期。
- `--self-test` 如何把真实网络交互收束为一个有限程序。

## 完整源码

```cpp
{{#include ../../examples/tcp_counter_server.cpp}}
```

[网络 API](../api/network.md) · [同步 API](../api/sync.md) · [超时 API](../api/time.md)
