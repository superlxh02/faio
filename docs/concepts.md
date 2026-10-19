# 核心概念

faio 的公开面只有三个名字：任务 `faio::task<T>`、把任务组合起来的并发接口、承载 I/O 结果的 `faio::expected<T>`。先把这三件事理清，再看具体模块。

## 1. 异步任务：`faio::task<T>`

返回 `faio::task<T>` 的函数就是一个协程。它同时具备三个性质，缺一不可。

### 协程

函数体里用 `co_await` 等结果、`co_return` 交结果，编译器把它拆成帧、等待点和续体。没有回调注册，也不需要 `future` / `promise` 两层胶水；`co_await` 之后的代码顺序就是执行顺序。

```cpp
#include <chrono>
using namespace std::chrono_literals;

faio::task<int> delayed_value(int value) {
  co_await faio::time::sleep(10ms);
  co_return value;
}
```

### 惰性

调用只构造任务对象，函数体一行都不执行：

```cpp
auto pending = delayed_value(42);               // 尚未开始
auto running = faio::spawn(std::move(pending)); // 这时才提交给运行时
const int value = co_await running;             // 挂起当前协程，等待结果
```

因此 `task` 可以安全地传递、拼接、存进容器，不会在构造点偷偷跑起来。任务与结果句柄都按移动所有权管理，句柄的结果只能领取一次。

### 异步

等待 I/O、定时器或同步原语时，协程挂起并把 worker 交还运行时；完成事件到达后，从 `co_await` 的下一行继续。等待期间不占线程，也不需要为每个等待配一个阻塞线程。

```cpp
faio::task<void> echo(faio::net::TcpStream stream) {
  char buf[1024];
  while (true) {
    auto n = co_await stream.read(buf);  // 挂起，线程去处理别的连接
    if (!n || *n == 0) break;
    if (!(co_await stream.write_all({buf, *n}))) break;
  }
}
```

示例以片段形式给出，完整可编译的版本见 [快速开始](getting-started.md) 与 [TCP 官方例程](examples/network.md)。

普通线程用 `block_on()` 进入运行时；协程内部只用 `co_await`。不要在 worker 内调用 `block_on()`、同步句柄 `get()` 或阻塞等待自己的任务。

## 2. 并发接口

任务本身不会自己开始，需要提交或组合：

| 接口 | 作用 | 完成边界 |
| --- | --- | --- |
| `spawn()` | 立即提交任务，返回结果句柄 | 句柄可等待、可取一次结果 |
| `join()` / `join_all()` | 并发启动多个分支，按参数顺序返回结果 | 所有分支退出后父协程才恢复 |
| `select()` | 等最先完成的分支，其余请求取消 | 胜者选出后，等其余分支排空再返回 |
| `scope` | 管理动态派生的子任务 | 离开作用域即汇合全部子任务 |

```cpp
auto [a, b] = co_await faio::join(delayed_value(20), delayed_value(22));
```

`join` 的组合状态直接放在父协程帧里，不堆分配、不做引用计数；代价是父任务返回前必须等所有分支退出——这段排空（drain）由库执行。`spawn_detached()` 不返回句柄，任务需要自己处理异常，并保证引用的数据活到任务结束。

## 3. 错误：`faio::expected<T>`

网络与文件操作返回 `faio::expected<T>`，即 `std::expected<T, faio::Error>` 的别名。检查结果再解引用，用 `error()` 读取失败原因：

```cpp
const auto count = co_await stream.read(buffer);
if (!count) {
  // 记录 count.error()，决定重试或结束连接。
  co_return;
}
if (*count == 0) co_return; // buffer 非空时表示 EOF
```

`Error` 保留错误域、原生错误码与已经传输的字节数——失败时也能知道读到了多少，组合算法据此决定是否重试剩余部分。

协程里的异常不走这条通道，而是沿等待链向上传播，可以用标准 `try/catch` 处理。时间等待和协作取消也可能以异常报告，具体以对应接口说明为准。

不要对未检查的 `expected` 直接使用 `*result` 或 `result->member`。

## 4. 生命周期与取消

异步操作可能持有调用方的缓冲区引用，直到操作最终完成。挂起期间不能让缓冲区析构、重分配或被其他任务无序修改。资源的关闭请求也不意味着所有在途操作已经完成。

协程 lambda 特别要注意捕获闭包的生命周期。官方例程优先采用无捕获 lambda，通过参数把值放进协程帧；引用参数仍须由调用方确保存活。

同一 stream 支持一个读方向和一个写方向操作同时进行；同方向冲突返回 `EBUSY`。空缓冲区读取成功不表示 EOF；只有非空读取返回零字节才能作为对端结束信号。

取消是协作协议：运行时通过停止请求通知任务，能响应的等待点完成取消；长计算和任意同步第三方函数不会自动中断。需要耗时同步调用时使用 `spawn_blocking()`，它把函数提交到独立的有界线程池，等待结果只挂起协程。

关闭是生命周期的一部分：在首次使用前调用 `runtime::configure()`；默认运行时首次使用时启动，`runtime::shutdown()` 排空后关闭，之后不能重新配置或重新启动。单线程模式由调用 `block_on()` 的线程驱动，未驱动时排队任务和异步事件不会自行推进。详细规则见 [运行时 API](api/runtime.md) 与 [运行时设计](异步运行时.md)。
