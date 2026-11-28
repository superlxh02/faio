# 同步与异步桥接

`block_on()` 让普通的 `main()` 进入异步世界。真实项目很少是全新的：已有的库可能只提供同步接口，主程序也可能由 GUI 框架的事件循环驱动。同步和异步之间需要明确的边界。

## 两个方向，两个工具

faio 提供两个方向相反的桥接工具：

- `block_on(task)`：从**同步**进入**异步**。它驱动任务直到完成，然后同步返回结果。
- `spawn_blocking(function)`：从**异步**回到**同步**。它把同步函数交给独立的阻塞线程池，等待时只挂起协程，不占 worker。

记住一句话：`block_on()` 是给同步入口用的，`spawn_blocking()` 是给协程里不得不跑的同步代码用的。

## `block_on()` 展开是什么

faio 没有 tokio 的 `#[tokio::main]` 宏，入口要自己写。`block_on()` 通常放在入口的最后：

```cpp
int main() {
  const auto result = faio::block_on(my_async_main());
  // result 已经是同步的值
  return 0;
}
```

它做的事是：找到当前默认运行时，进入异步上下文，驱动传入的根任务，等任务完成后把结果带回当前线程。任务体抛出的异常会在这里按原类型重抛。

有一条硬规矩：**不能在运行时 worker 上调用 `block_on()`**。协程里想等待另一个任务，用 `co_await` 或 `spawn()`，不要退化成阻塞等待。实现里会直接抛 `std::logic_error`。

## 把同步接口包装成异步

假设有一个库存查询库，只提供同步函数，内部还会 `sleep_for`：

```cpp
int query_stock() {
  std::this_thread::sleep_for(50ms);
  return 6;
}
```

如果直接在协程里调用它，`sleep_for` 会让整个 worker 睡 50 毫秒。这段时间里，排在这个 worker 上的其他任务全都被卡住。正确做法是扔进阻塞池：

```cpp
faio::task<int> inventory_value() {
  auto stock = faio::spawn_blocking(query_stock);
  const int price = co_await query_price();   // 异步查询，和上面并发
  const int units = co_await stock;           // 这时再去领同步查询的结果
  co_return units * price;
}
```

`spawn_blocking()` 立即把函数提交出去，返回一个可等待的句柄。注意这里把两件事重叠了：同步查询在阻塞池里跑的同时，异步的价格查询也在推进。等 `co_await stock` 时，同步查询多半已经结束了。

完整代码见 [`examples/runtime/execution.cpp`](../../examples/runtime/execution.cpp)。

## 阻塞池怎么配置

阻塞池和协程 worker 是两套线程。默认用弹性策略，从零线程开始，有任务才创建，空闲一段时间后回收。可以改成固定大小，并调整上限：

```cpp
faio::runtime::configure(faio::config_builder{}
                             .set_blocking_pool_policy(
                                 faio::runtime::blocking_pool_policy::FIXED)
                             .set_max_blocking_threads(8)
                             .build());
```

还有 `set_blocking_keep_alive()` 控制空闲线程保留多久，`set_blocking_queue_limit()` 限制排队任务数。和运行时配置一样，这些必须在第一次 `block_on()` 或 `spawn()` 之前设置。

## 把异步包装成同步接口

反过来也有用：你写了一个异步库，但调用方是同步代码。可以把运行时句柄存起来，在接口里调 `block_on()`：

```cpp
class SyncClient {
 public:
  int get(std::string_view key) {
    return runtime_.block_on(fetch(key));
  }

 private:
  faio::runtime::current_thread_runtime runtime_;
};
```

这种方式适合把异步能力隔离在项目的一小块里。 GUI 程序常这么组织：GUI 代码留在主线程，运行时放到另一个线程上，两者通过通道或 `block_on()` 交互。

要注意的是：不要在高并发的同步调用路径上这么做。每次调用都 `block_on()` 会让异步的优势荡然无存。

## 什么时候用 `spawn_blocking`

判断标准是：**这段同步代码会不会长时间占用线程**。

适合放进阻塞池的：文件压缩、加密计算、调用只提供同步接口的第三方库、DNS 之外的阻塞解析。

不适合放进去的：只是几行内存操作。提交到阻塞池本身有开销，太轻的任务不值得。另外，阻塞池里的函数不能 `co_await`，它就是一段普通的同步代码。

## 运行看看

```sh
cmake --build build
./build/my_app
```

`examples/runtime/execution.cpp` 会打印 `库存总价值：42`。

## 下一步

桥接解决了"同步代码怎么进来"的问题。下一个工程问题是"怎么让程序停下来"：怎么通知所有任务退出，又怎么确认它们真的退干净了。见[优雅停机](shutdown.md)。


[上一章：协程并发](concurrency.md) · [下一章：优雅停机 →](shutdown.md)
