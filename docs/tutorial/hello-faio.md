# Hello faio

这一章写一个完整的异步程序：它启动两个任务并发工作，等它们都完成后把结果加起来。代码很短，但把 faio 最核心的三个东西——任务、`co_await` 和组合子——都用上了。

## 从一段代码开始

这是我们最终要跑的程序，也就是仓库里的 `examples/quickstart.cpp`：

```cpp
#include <faio/faio.hpp>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

faio::task<int> delayed_value(int value) {
  co_await faio::time::sleep(10ms);
  co_return value;
}

faio::task<int> answer() {
  auto first = faio::spawn(delayed_value(20));
  const auto a = co_await first;

  auto [b, c] = co_await faio::join(delayed_value(11), delayed_value(11));
  co_return a + b + c;
}

int main() {
  const auto result = faio::block_on(answer());
  std::cout << "The answer is " << result << '\n';
  return result == 42 ? 0 : 1;
}
```

运行它，会得到 `The answer is 42`。现在我们把这段代码拆开看。

## `task<T>` 是惰性的

`delayed_value` 是一个协程函数，返回 `faio::task<int>`。这里最容易踩的坑是：**调用协程函数不会执行函数体**。

```cpp
auto pending = delayed_value(42);  // 任务创建了，但函数体一行都没跑
```

`task` 是惰性且移动独占的。它不可复制，同一份任务也只能等待一次。要让它跑起来，得用 `co_await` 等待它，或者用 `spawn()` 提交给运行时。具名的任务在等待时要用 `std::move` 明确交出所有权：

```cpp
co_await std::move(pending);
```

直接等待函数调用返回的临时任务就不用 `move`，因为临时值本身就是右值：`co_await delayed_value(42)`。

## `co_await` 到底做了什么

`co_await faio::time::sleep(10ms)` 这一句的意思是：等 10 毫秒。但等的方式很关键。

如果这是同步的 `sleep`，线程会睡过去，什么也干不了。而 `co_await` 会让当前协程挂起：运行时把这个协程记下来，转头去执行别的任务；10 毫秒到了，再把它唤醒，从 `co_await` 这一句接着往下走。

所以 `co_await` 看起来像在"等"，实际上线程一秒都没闲着。这就是异步的核心。要验证这一点，可以在等待期间提交另一个任务——它会在第一个任务睡觉的时候跑完。我们下一章就用 `spawn()` 做这件事。

## `spawn()` 让任务并发跑起来

回到 `answer()`：

```cpp
auto first = faio::spawn(delayed_value(20));
const auto a = co_await first;
```

`spawn()` 把任务立即提交给运行时，然后立刻返回一个 `join_handle<int>`。注意和直接 `co_await delayed_value(20)` 的区别：

- 直接等待：父协程调用子协程，子协程跑完把执行权交回父协程，一个接一个。
- `spawn()`：任务交给调度器排队，可以在别的 worker 上跑；父协程之后再拿着句柄领结果。

`join_handle` 可以等待，也能请求停止。这里我们先用它领结果，`request_stop()` 留到[深入 task](task-in-depth.md) 那一章讲。

## `join()` 一次等多个任务

接下来这一段：

```cpp
auto [b, c] = co_await faio::join(delayed_value(11), delayed_value(11));
```

`join()` 接受多个任务，返回一个新任务。等到所有分支都结束，它按**参数顺序**把结果打包成 tuple。所以 `b` 是第一个 11，`c` 是第二个 11，跟谁先跑完无关。

和 `spawn()` 不同，`join()` 本身只是构造了一个惰性任务。真正让分支跑起来的是外面的 `co_await`。分支内部仍是并发推进的——两个 `delayed_value(11)` 会一起睡那 10 毫秒，而不是睡两次。

任务数量不固定的时候用 `join_all()`，它接受一个 `std::vector<task<T>>`，返回 `std::vector<T>`，顺序同样与输入对应。

## `block_on()` 把异步结果带回同步世界

最后：

```cpp
const auto result = faio::block_on(answer());
```

`main()` 是普通函数，不能写 `co_await`。`block_on()` 就是给这种同步入口用的：它驱动任务直到完成，然后同步返回结果。如果任务体抛了异常，会在这里按原类型重新抛出。

有一点要记住：**在运行时 worker 内部不能调用 `block_on()`**。task 里要组合别的任务，用 `co_await` 或 `spawn()`，不要退化成阻塞等待。

## 运行看看

在工程目录里构建并运行：

```sh
cmake --build build
./build/my_app
```

输出应该是：

```text
The answer is 42
```

如果结果是别的数字，回头检查 `join()` 的解构顺序，以及每个 `delayed_value()` 的参数。

## 下一步

现在你知道了任务怎么创建、怎么等待、怎么并发组合。接下来我们让多个网络并发处理：客户端连进来之后，每个连接交给一个独立的任务，谁也不阻塞谁。这就是[生成任务](spawning.md)那一章的内容。

---

[上一章：环境准备](setup.md) · [下一章：生成任务 →](spawning.md)
