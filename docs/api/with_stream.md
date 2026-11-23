# with_stream

`with_stream` 让生产者按消费者的拉取节奏交付数据，消费端提前退出时收束生产者。头文件 `<faio/coroutine/sequence.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `with_stream(factory, body)` | 返回 body 的结果；生产者按需 emit，消费者逐项 next |
| `stream_producer<T, S>::emit(value)` | 交付一项后等待下一次拉取许可，容量为一，不预取 |
| `stream_reader<T, S>::next()` | 拉取下一项，返回 optional；生产正常结束时为 `nullopt` |
