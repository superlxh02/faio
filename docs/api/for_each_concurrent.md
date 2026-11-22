# for_each_concurrent

消费异步序列并且不想逐项串行处理时用它。它从流中拉取元素，交给最多 `limit` 个并发处理任务，并在返回前收束所有处理。先包含 `<faio/coroutine/sequence.hpp>`。

```cpp
template <class T, class S, class Factory>
  requires std::same_as<std::invoke_result_t<Factory&, T>, task<void, S>>
task<void, S> for_each_concurrent(std::size_t limit, stream_reader<T, S>& input,
                                  Factory operation);

template <class T, class S = default_scheduler, class StreamFactory, class OperationFactory>
task<void, S> for_each_concurrent(std::size_t limit, StreamFactory factory,
                                  OperationFactory operation);
```

借用读端的版本接收已经存在的 `stream_reader`，通常在 `with_stream` 的主体里调用。自持流的版本接收流工厂，在返回前完成生产者的停止与排空。`operation` 按值持有，多个处理任务可以并行调用它，闭包里的共享状态需要自行同步。

并发窗口把进行中的 `next()` 也算在内：从拉取到处理完成合计最多 `limit` 项。值为 0 或超过 `std::ptrdiff_t` 上限时，等待该任务会抛出 `std::invalid_argument`。

任何一项处理抛出异常，组合会停止其余处理和待完成的拉取，排空已经接纳的操作后再把首个异常交给等待方。因停止请求产生的 `operation_cancelled` 不会被当成处理失败。外层 `with_stream` 随后停止并排空生产者。

`limit` 与流的总长度无关，流可以无限长；窗口只限制同时在途的项数。

这个接口只保留首个失败，不累积全部结果。并发度受 `limit` 限制，窗口越大，同时在途的数据和内存越多。

完整程序见 [异步序列](../../examples/coroutine/sequence.cpp)。
