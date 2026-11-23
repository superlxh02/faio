# for_each_concurrent

`for_each_concurrent` 用固定窗口并发消费异步序列，不逐项串行等待。头文件 `<faio/coroutine/sequence.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `for_each_concurrent(limit, reader, operation)` | 借用已有 `stream_reader`，按并发窗口处理元素 |
| `for_each_concurrent(limit, factory, operation)` | 自持流，在返回前完成生产者停止与排空 |
