# 专题

[教程](../learn-faio.md)以 quickstart 和 tcp_echo 两个示例介绍入门。专题围绕具体的工程问题展开：任务如何建模，多个任务怎样并发，同步代码怎样接入，服务怎样停下，错误怎样表达和处理。可以按顺序读，也可以直接跳到当前问题。

| 专题 | 解决的问题 |
| --- | --- |
| [faio::task\<T\>](task.md) | 任务何时开始执行，结果、异常和取消分别放在哪里 |
| [协程并发](concurrency.md) | 多个任务怎样组合、等待、竞速和收束 |
| [同步与异步桥接](bridging.md) | 已有同步代码怎样接入协程，`block_on()` 和 `spawn_blocking()` 怎样选 |
| [优雅停机](shutdown.md) | 怎样通知任务退出，又怎样确认它们已经收尾 |
| [错误处理](errors.md) | 用条件判断或 `match` 处理 `expected`，区分错误值、异常和取消 |

碰到术语不清楚，看[术语表](../glossary.md)。想查具体接口，看[API 介绍](../api/index.md)或[API 查询](../api-reference.md)。
