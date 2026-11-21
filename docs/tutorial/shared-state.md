# 共享状态

上一章的服务能并发处理连接了，但每个连接各干各的。这一章我们加一份所有请求共享的配置：它在后台加载，加载完成后通知所有等待的请求；同时用信号量限制同时处理的请求数量。

## 两种共享状态的办法

在 faio 里共享状态有两条路：

1. 用 `mutex` 保护共享数据，直接读写；
2. 用一个专门的任务持有状态，其他地方通过消息传递来操作它。

简单的、不需要异步的数据用第一种；涉及 I/O 或异步操作的用第二种。我们这里的配置就是一个 `std::optional<std::string>`，读写都不涉及 I/O，所以用 `mutex` 就够了。消息传递的办法留到[下一章](channels.md)讲。

## 定义共享对象

把锁、条件变量和数据放在同一个结构里：

```cpp
#include <faio/faio.hpp>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

struct SharedConfig {
  faio::sync::mutex mutex;
  faio::sync::condition_variable changed;
  std::optional<std::string> value;
};
```

`value` 为空表示配置还在加载，填入之后就保留发布的配置。这三个对象必须由所有等待者共享，并且活到最后一个任务结束。

## 发布配置

加载任务先睡一小会儿模拟耗时，然后拿到锁写入数据，最后通知等待者：

```cpp
faio::task<void> publish(SharedConfig& config) {
  co_await faio::time::sleep(5ms);
  {
    auto guard = co_await config.mutex.scoped_lock();
    config.value.emplace("catalog");
  }  // guard 在这里析构，锁被释放
  config.changed.notify_all();
}
```

注意大括号的作用：它让 `guard` 在调用 `notify_all()` **之前**就析构。如果锁还攥在手里就通知，被唤醒的任务会立刻去抢锁，却只能干等，白白多一次调度。

`scoped_lock()` 等待后返回一个 RAII 守卫，离开作用域自动解锁。这是推荐用法。如果想自己控制，`lock()` 返回一个等待对象，`unlock()` 手动释放。

## 等待条件成立

请求任务要等配置就绪。这里用条件变量的 `wait`，它接受一个谓词：

```cpp
faio::task<std::string> request(SharedConfig& config,
                                faio::sync::semaphore& slots,
                                int request_id) {
  std::string name;
  {
    auto guard = co_await config.mutex.scoped_lock();
    co_await config.changed.wait(config.mutex,
                                 [&] { return config.value.has_value(); });
    name = *config.value;
  }

  auto permit = co_await slots.acquire_permit();
  co_await faio::time::sleep(10ms);
  co_return name + "-" + std::to_string(request_id);
}
```

`wait(mutex, predicate)` 做三件事：检查谓词，满足就直接继续；不满足就释放 `mutex` 并挂起协程；被通知后重新拿到 `mutex`，再检查谓词。返回时锁是重新持有的，所以这一整套要放在守卫的作用域内。

如果发布任务先跑完了，谓词第一次检查就成立，`wait` 不会挂起。这避免了"通知发得太早没人收到"的经典问题。

## 用信号量限制并发

接下来这段：

```cpp
auto permit = co_await slots.acquire_permit();
co_await faio::time::sleep(10ms);
```

`acquire_permit()` 取得一个并发配额，返回 RAII 的 `permit`。任务退出时 permit 析构，配额自动归还。这里只持有配额，不持有 `mutex`——异步等待期间攥着锁会挡住别的请求，是典型的错误写法。

信号量的容量在创建时指定：

```cpp
faio::sync::semaphore slots{2};  // 最多两个请求同时执行
```

`release()` 可以手动归还配额，`acquire()` 取得后不自动归还。想让 RAII 帮你管理，就用 `acquire_permit()`。

## 把任务拼起来

最后用一个 `join()` 同时启动发布者和三个请求：

```cpp
faio::task<void> demonstrate() {
  SharedConfig config;
  faio::sync::semaphore slots{2};
  const auto [first, second, third, published] =
      co_await faio::join(request(config, slots, 1),
                          request(config, slots, 2),
                          request(config, slots, 3),
                          publish(config));
  (void)published;
  std::cout << first << ", " << second << ", " << third << '\n';
}
```

`join()` 等所有分支退出，按参数顺序交付结果。四个分支里三个是 `std::string`，一个是 `void`——`void` 分支在 tuple 里用 `std::monostate` 占位，所以这里得把 `published` 接住。

运行结果是三行带配置的报价：

```text
catalog-1, catalog-2, catalog-3
```

## 同步原语不会占住线程

这是 faio 的同步原语和标准库最大的区别。`std::mutex` 抢不到锁时，线程阻塞在那里；`faio::sync::mutex` 抢不到锁时，协程挂起，worker 转去执行别的任务。等锁空出来了，协程再被唤醒。

所以你可以在协程里放心地用这些原语，不用担心一个等待的请求会把整个 worker 卡死。当然，临界区里仍然只应该放同步代码，不要在里面做 I/O。

本章完整代码见 [`examples/sync/shared_state.cpp`](../../examples/sync/shared_state.cpp)。

## 运行看看

```sh
cmake --build build
./build/my_app
```

## 下一步

`mutex` 适合保护简单数据。但如果状态本身需要异步操作，或者你想把结果从很多任务汇集到一个地方，通道更合适。下一章我们用 `mpsc`、`latch` 和 `barrier` 做任务协调，看看[通道](channels.md)怎么用。

---

[上一章：生成任务](spawning.md) · [下一章：通道 →](channels.md)
