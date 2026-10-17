# 协程基础

一个文件依次演示六个场景：直接等待子协程、启动并发任务、后台通知、固定分支汇合、动态任务集合与超时竞速。

## 构建与运行

先按 [快速开始](../getting-started.md) 配置工程：

```sh
cmake --build build/local --target coroutine_basics
./build/local/examples/coroutine_basics
```

程序依次输出六种任务组织方式的结果，然后自动退出，不需要网络客户端。

## 阅读重点

- `example_await_and_block_on()`：直接等待子任务不等于创建独立并发任务。
- `example_spawn_and_join_handle()`：`spawn` 提交之后，等待句柄的先后顺序不改变已经启动的并发。
- `example_spawn_detached()`：完成信号覆盖后台任务的生命周期，任务内部处理异常。
- `example_join()` / `example_join_all()`：分别用于固定异构分支与动态同构任务集合。
- `example_select()`：获胜分支的下标对应原始参数，并排空其他分支。

可以把最后一个例程中的查询延时改成 `200ms`，观察超时分支获胜。操作取消是协作式的，不能据此推断任意同步计算都能被立即中断。

## 完整源码

```cpp
{{#include ../../examples/coroutine_basics.cpp}}
```

[API：任务与并发](../api/tasks.md) · [深入：协程并发](../协程并发.md)
