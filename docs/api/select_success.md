# select_success

公共头文件为 `<faio/coroutine/composition.hpp>`。

```cpp
auto select_success(task<Ts, S>... children)
```

## 语义与限制

首个成功槽构造完毕后仲裁，再停止并排空其余任务。全失败仍按输入顺序保存诊断。

## 成本

每项一个结果槽和工作根。返回包含下标，重复值类型按下标读取。

完整程序见 [进阶竞速](../../examples/coroutine/race_strategies.cpp)。
