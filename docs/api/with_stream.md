# with_stream

需要把生产者产生的多项数据按需交给消费者，并在消费端提前退出时收束生产者，用 `with_stream`。它提供一个单消费者读端和一个显式 `emit` 的生产端，容量为一，不预取。先包含 `<faio/coroutine/sequence.hpp>`。

```cpp
template <class T, class S = default_scheduler, class Factory, class Body>
auto with_stream(Factory factory, Body body)
    -> std::invoke_result_t<Body&, stream_reader<T, S>&>;
```

生产者工厂接收 `stream_producer<T, S>&`，按顺序 `co_await output.emit(value)`。`emit` 交付一项后等待下一次拉取许可，因此生产者不会跑在消费者前面。主体接收 `stream_reader<T, S>&`，用 `co_await input.next()` 拉取；返回 `std::optional<T>`，生产正常结束时为 `nullopt`，生产者抛出的异常原样传播。

读端是单消费者、不可复制的，同一时刻只允许一个 `next()` 挂起。`stream_reader` 只能在这个 `body` 的词法作用域内使用，不能保存到流组合结束之后。

`body` 的返回值就是 `with_stream` 的返回值，类型由主体推断。生产者工厂按值移动进组合帧，只有开始等待时才启动。

拉取和关闭都要求当前调度器与流所属调度器一致，否则抛出 `std::logic_error`。同时发起两个 `next()` 也会被拒绝。

主体退出时，流组合请求生产者停止，等待进行中的 `next` 返回，再排空生产者协程。主体失败且生产者随后也失败时，等待方收到 `cleanup_error`；只有一侧失败时传播该侧原异常。主体提前返回是正常结束，生产者收到的取消不算失败。

每项数据经过一次信号量交接，容量固定为一，生产者和消费者不能并行处理同一项。逐项处理，或者把读端交给 `for_each_concurrent` 展开并发，都是合适的使用方式。

`co_yield` 生成器是另一套接口，不能用在这里的生产者中。

生产者正常返回表示流结束，消费者在后续 `next()` 中读到 `nullopt`。

完整程序见 [异步序列](../../examples/coroutine/sequence.cpp)。
