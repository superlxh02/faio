# 通道

上一章用 `mutex` 共享了一份简单配置。这一章换个场景：几个任务各自算出结果，我们想把结果汇集到一处统一处理。这里用有界 MPSC 通道，再配上 `latch` 和 `barrier` 控制启动时机和工作节奏。

## 这一章要做什么

我们模拟两个分片处理数据：先由一个准备任务写入输入，两个生产任务各自等待输入就绪，然后分两轮算出结果，通过一个容量有限的通道发给消费者。消费者把所有结果加起来。

这里有三个协调问题要解决：

1. 生产任务要等输入准备好——用 `latch`；
2. 两个分片每轮要对齐进度——用 `barrier`；
3. 结果要汇集到一个地方——用 `mpsc` 通道。

## 有界 MPSC 通道

通道用 `make(capacity)` 创建，返回一对端点：

```cpp
using ResultChannel = faio::sync::mpsc<int>;

auto [sender, receiver] = ResultChannel::make(1);
```

发送端可以复制，接收端移动独占。容量必须大于零。这里的容量是 1——通道满时发送协程会挂起，等消费者腾出空间再继续。这正是有界通道的意义：生产太快时不会无限堆积内存，而是自然地把压力回传给生产者。

## 发送和接收

生产任务按轮次发送结果：

```cpp
faio::task<void> produce(faio::sync::latch& ready,
                         faio::sync::barrier& phase,
                         const int& input,
                         ResultChannel::sender output) {
  co_await ready.wait();
  for (const int factor : {1, 2}) {
    const int result = input * factor;
    const auto sent = co_await output.send(result);
    if (!sent)
      throw std::runtime_error("发送结果失败：" +
                               std::string{sent.error().message()});
    co_await phase.arrive_and_wait();
  }
  output.close();  // 最后一个发送端关闭后，接收端能观察到通道关闭
}
```

`send()` 的结果是 `expected<void>`。通道满时 `co_await` 会挂起，有空位了再继续。`close()` 释放这个发送端；当所有发送端都关闭、队列也排空之后，接收端会读到关闭错误。

消费者一直读到通道关闭为止：

```cpp
faio::task<int> consume(ResultChannel::receiver input) {
  int count = 0;
  int total = 0;
  for (;;) {
    const auto result = co_await input.recv();
    if (!result) {
      if (result.error().value() != faio::Error::ClosedChannel)
        throw std::runtime_error("接收结果失败：" +
                                 std::string{result.error().message()});
      break;
    }
    ++count;
    total += *result;
  }
  if (count != 4)
    throw std::runtime_error("收到的结果数量错误");
  co_return total;
}
```

`recv()` 在队列为空时挂起，两项结果到达就被唤醒。队列空且已关闭时，返回 `Error::ClosedChannel`——注意区分"暂时没数据"和"以后都不会有数据了"。`try_recv()` 可以在同步代码里查，它返回 `expected<optional<T>>`，把这两种情况分开表达。

## latch：一次性的起跑信号

准备任务写入输入后，把 latch 归零：

```cpp
faio::task<void> prepare(std::array<int, 2>& inputs, faio::sync::latch& ready) {
  inputs = {10, 11};
  ready.count_down();
  co_return;
}
```

生产任务在一开头 `co_await ready.wait()`。latch 初始计数是 1，归零后所有等待者立即通过。这是个一次性开关——归零之后就一直是打开状态，不能再用。

`latch` 适合"等到某件事发生"；如果只是想让一组任务都到达某个点再一起走，用 `barrier`。

## barrier：分轮对齐

`phase.arrive_and_wait()` 让两个分片对齐：

```cpp
for (const int factor : {1, 2}) {
  // 发送本轮结果
  co_await phase.arrive_and_wait();  // 等另一个分片也发完
}
```

barrier 创建时指定参与人数 `faio::sync::barrier phase{2}`。每个参与者调用 `arrive_and_wait()` 后挂起，直到两个参与者都到齐，才一起放行进入下一轮。这样每一轮的结果都是成对的。

## 用任务组收束

最后把所有任务放进任务组：

```cpp
faio::task<void> demonstrate() {
  std::array<int, 2> inputs{};
  faio::sync::latch ready{1};
  faio::sync::barrier phase{2};
  auto [sender, receiver] = ResultChannel::make(1);
  auto second_sender = sender;

  auto result = co_await faio::task_group([&](faio::task_group_context& group)
      -> faio::task<int> {
    (void)group.spawn_observed(prepare(inputs, ready));
    (void)group.spawn_observed(
        produce(ready, phase, inputs[0], std::move(sender)));
    (void)group.spawn_observed(
        produce(ready, phase, inputs[1], std::move(second_sender)));
    auto collector = group.spawn_observed(consume(std::move(receiver)));
    co_return (co_await collector).value_or_throw();
  });
  const int total = std::move(result.body).value_or_throw();
  if (total != 63)
    throw std::runtime_error("两个分片的汇总结果错误");
  std::cout << "全部分片处理完毕，结果之和为 " << total << '\n';
}
```

`task_group` 做的是结构化并发：它接纳的任务都在词法作用域内，等主体返回后，组会排空所有还没退出的孩子，再返回结果。即使某个任务失败，组也会请求兄弟任务停止并等它们收尾，不会留下悬空的工作。

`spawn_observed()` 返回的是 `task_outcome<T>` 句柄，`value_or_throw()` 取出结果或重抛异常。两个发送端要分别 move 进两个生产任务，最后一个生产任务关闭后，消费者才能看到通道关闭。

结果是四个数字之和：(10×1 + 10×2) + (11×1 + 11×2) = 30 + 33 = 63。

## 运行看看

```sh
cmake --build build
./build/my_app
```

应该看到：

```text
全部分片处理完毕，结果之和为 63
```

本章完整代码见 [`examples/sync/coordination.cpp`](../../examples/sync/coordination.cpp)。

## 下一步

到现在为止，我们一直在讲"怎么用"这些接口。下一章换个角度：把 TCP 回显服务真正写完，仔细看看 `TcpStream` 的读写接口——为什么读有 `read` 和 `read_exact` 两个版本，EOF 怎么判断，超时挂在哪里。这就是[网络 I/O](io.md)。

---

[上一章：共享状态](shared-state.md) · [下一章：网络 I/O →](io.md)
