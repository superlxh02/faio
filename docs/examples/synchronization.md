# 同步与阻塞任务

## 六种协程同步原语

`coroutine_sync` 用库存更新、配置加载、并发限流、初始化、分轮处理与订单队列，展示 mutex、condition_variable、semaphore、latch、barrier 和 mpsc。

```sh
cmake --build build/local --target coroutine_sync
./build/local/examples/coroutine_sync
```

程序中的等待挂起协程，争用期间 worker 仍可以处理其他任务。阅读时关注：共享数据在哪把锁下访问、对象是否覆盖所有等待者的生命周期，以及停止 / 关闭后如何退出。

### 完整源码：协程同步

```cpp
{{#include ../../examples/coroutine_sync.cpp}}
```

## 阻塞函数与异步任务协作

`blocking_thread_pool` 把模拟同步 SDK 调用放入 `spawn_blocking()`，同时执行异步价格查询与协程心跳。

```sh
cmake --build build/local --target blocking_thread_pool
./build/local/examples/blocking_thread_pool
```

观察同步函数等待期间，异步心跳仍持续输出。线程池让阻塞工作离开 worker，但不能使第三方同步函数自动具备可取消性。

### 完整源码：阻塞任务

```cpp
{{#include ../../examples/blocking_thread_pool.cpp}}
```

[API：协程同步](../api/sync.md) · [运行时配置](../api/runtime.md) · [同步模块设计](../协程同步.md)
