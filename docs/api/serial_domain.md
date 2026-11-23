# serial_domain

`serial_domain` 让跨挂起点的多项操作互斥执行。头文件 `<faio/coroutine/composition.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `serial_domain::run(factory)` | 取得域后调用无参工厂并等待其 `task<R, S>`；返回同类型任务 |
| `close()` | 关闭接纳并等待所有已进入或已排队的调用退出 |
