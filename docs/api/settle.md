# settle

公共头文件为 `<faio/coroutine/composition.hpp>`。

```cpp
task<task_outcome<T>, S> settle(task<T, S> work)
```

## 语义与限制

捕获值、异常和协作停止。单次消费支持 move-only。判断 operation_cancelled 需要当前工作令牌已收到停止。

## 成本

无异常移动的值就地存储，可能抛出的移动使用独立所有权。

完整程序见 [批处理与任务终态](../../examples/coroutine/batch_results.cpp)。
