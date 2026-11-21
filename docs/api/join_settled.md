# join_settled

公共头文件为 `<faio/coroutine/composition.hpp>`。

```cpp
auto join_settled(task<Ts, S>... children)
```

## 语义与限制

返回每项终态，收集硬错误也不主动停止兄弟工作。动态同型输入使用 join_all_settled。

## 成本

真实子工作逐项需要根及结果元数据。

完整程序见 [批处理与任务终态](../../examples/coroutine/batch_results.cpp)。
