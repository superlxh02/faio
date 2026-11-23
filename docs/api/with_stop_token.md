# with_stop_token

`with_stop_token` 给局部工作再接入一个 `std::stop_token`，父任务和外部来源都能请求停止。头文件 `<faio/coroutine/lifecycle.hpp>`。

## 接口

| 接口 | 行为 |
| --- | --- |
| `with_stop_token(token, work)` | 返回惰性 `task<T, S>`；父任务停止连接保持不变，传入令牌作为额外来源接入 |
| `work` | 原任务的值、异常和取消按原生规则交付 |
