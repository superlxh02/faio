# API 介绍

<div class="api-eyebrow">PUBLIC API · C++23</div>

[打开核心公开 API 的 Doxygen 参考 →](../../api/index.html)

Doxygen 参考覆盖任务、运行时、网络、文件、时间、同步、流操作以及错误与日志等核心公开 API，列出每个收录类型和函数的重载签名、参数、返回值与生命周期约定。TLS、异步进程和实验层不进 Doxygen，它们的调用方式分别见 [TLS 介绍](tls.md)、[进程介绍](process.md)和[实验性接口介绍](experimental.md)。

<div class="api-grid">
<a class="api-card" href="../../api/group__tasks.html"><span>faio</span><h3>协程与并发 <b>↗</b></h3><p>task、spawn、join、select 和 scope。</p><code>co_await faio::join(...)</code></a>
<a class="api-card" href="../../api/group__runtime.html"><span>faio::runtime</span><h3>运行时与配置 <b>↗</b></h3><p>调度模式、并发容量与运行时生命周期。</p><code>runtime::configure(...)</code></a>
<a class="api-card" href="../../api/group__network.html"><span>faio::net</span><h3>网络 I/O <b>↗</b></h3><p>TCP、UDP、地址解析与连接操作。</p><code>TcpStream · TcpListener</code></a>
<a class="api-card" href="../../api/group__filesystem.html"><span>faio::fs</span><h3>文件与目录 <b>↗</b></h3><p>文件读写、元数据、路径与目录枚举。</p><code>File · ReadDir · OpenOptions</code></a>
<a class="api-card" href="../../api/group__time.html"><span>faio::time</span><h3>时间与超时 <b>↗</b></h3><p>休眠、周期等待与 I/O 截止时间。</p><code>sleep · interval · timeout</code></a>
<a class="api-card" href="../../api/group__sync.html"><span>faio::sync</span><h3>协程同步 <b>↗</b></h3><p>互斥锁、信号量、条件变量与有界通道。</p><code>mutex · semaphore · mpsc</code></a>
<a class="api-card" href="../../api/group__io.html"><span>faio::io</span><h3>缓冲与流操作 <b>↗</b></h3><p>缓冲区所有权、完整传输和读写适配器。</p><code>io_buffer · BufReader</code></a>
<a class="api-card" href="../../api/group__errors.html"><span>faio · faio::log</span><h3>错误与日志 <b>↗</b></h3><p>检查操作结果，读取错误和传输进度。</p><code>expected&lt;T&gt; · Error</code></a>
<a class="api-card" href="tls.html"><span>faio::tls</span><h3>TLS 加密 <b>↗</b></h3><p>证书、连接握手与加密字节流。</p><code>TlsConnector · TlsStream</code></a>
<a class="api-card" href="process.html"><span>faio::process</span><h3>异步进程 <b>↗</b></h3><p>命令、标准流与进程回收。</p><code>Command · Child</code></a>
<a class="api-card" href="experimental.html"><span>faio::experimental</span><h3>实验性接口 <b>↗</b></h3><p>异步入口注解与 Sender/Receiver 互操作。</p><code>async_main · as_sender</code></a>
</div>

## 包含哪个头文件

### 协程与运行时

| 公共头文件 | 适用场景 |
| --- | --- |
| `<faio/coroutine/task.hpp>` | 定义任务、查询当前协程、使用任务属性和生命周期回调 |
| `<faio/coroutine/composition.hpp>` | 汇合、竞速、任务组、共享结果和串行执行域 |
| `<faio/coroutine/awaiter.hpp>` | 把应用自己的异步操作接入协程 |
| `<faio/coroutine/lifecycle.hpp>` | 清理、协作停止和任务截止 |
| `<faio/coroutine/sequence.hpp>` | 生成、消费和并发处理异步序列 |
| `<faio/runtime/default.hpp>` | 配置、驱动、提交任务和关闭默认运行时 |
| `<faio/runtime/instance.hpp>` | 创建独立运行时和跨实例执行任务 |

### IO、网络与文件系统

| 公共头文件 | 适用场景 |
| --- | --- |
| `<faio/io/stream.hpp>` | 通用流算法、缓冲读写、拥有型缓冲区和内存流 |
| `<faio/io/native.hpp>` | 接入已有 Unix 文件描述符 |
| `<faio/net/address.hpp>` | 构造、解析地址和查询主机名 |
| `<faio/net/tcp.hpp>` | 建立、监听和读写 TCP 连接 |
| `<faio/net/udp.hpp>` | 创建 UDP 套接字并收发数据报 |
| `<faio/net/local.hpp>` | Unix 域套接字与本地管道 |
| `<faio/fs/file.hpp>` | 打开文件、读写、定位和同步 |
| `<faio/fs/directory.hpp>` | 逐条枚举目录 |
| `<faio/fs/path.hpp>` | 路径和元数据查询，以及文件、目录的路径操作 |

### 同步、时间、诊断与可选组件

| 公共头文件 | 适用场景 |
| --- | --- |
| `<faio/sync/mutex.hpp>` | 互斥锁、RAII 锁与条件变量 |
| `<faio/sync/coordination.hpp>` | 信号量、闩与屏障 |
| `<faio/sync/channel.hpp>` | 在任务间传送消息的 MPSC 通道 |
| `<faio/time/timer.hpp>` | 休眠、周期定时器和 I/O 超时 |
| `<faio/common/error.hpp>` | 应用接口中直接使用的错误与结果类型 |
| `<faio/log/logger.hpp>` | 获取 faio 日志记录器和调整日志级别 |
| `<faio/tls/handshake.hpp>` | 配置证书信任，发起或接纳握手 |
| `<faio/tls/stream.hpp>` | 读写已建立的 TLS 连接 |
| `<faio/process/command.hpp>` | 配置和启动命令、等待退出或捕获输出 |
| `<faio/process/child.hpp>` | 操作已有子进程的标准流、等待或终止进程 |

每个功能头都会一并提供其接口签名用到的类型。例如 `<faio/net/tcp.hpp>` 会带出 TCP 读写所需的地址和缓冲区类型，不必单独包含。

想要全部稳定功能，直接包含 `<faio/faio.hpp>`；想控制编译开销，按需包含上表中的 `<faio/模块/功能.hpp>` 即可。TLS 和进程组件需要先用开关启用，见 [TLS](tls.md) 和[异步进程](process.md)；[实验接口](experimental.md)有单独的入口。

从旧版本升级时，按上表把原来直接位于 `faio/` 下的功能头替换为新路径。

## 组合器与任务终态

`<faio/coroutine/composition.hpp>` 提供的组合接口和结果类型各自有说明页：

| 主题 | 页面 |
| --- | --- |
| 汇总与限并发 | [join_settled](join_settled.md)、[join_all_settled](join_all_settled.md)、[join_all_limited](join_all_limited.md) |
| 竞速 | [select_success](select_success.md)、[race_in](race_in.md) |
| 终态与清理 | [settle](settle.md)、[with_cleanup](with_cleanup.md)、[with_stop_token](with_stop_token.md)、[complete_before](complete_before.md) |
| 任务组 | [task_group](task_group.md) |
| 序列与调度 | [with_stream](with_stream.md)、[for_each_concurrent](for_each_concurrent.md)、[on](on.md)、[serial_domain](serial_domain.md)、[shared_task](shared_task.md) |

`task_outcome`、`group_result`、`select_result` 等结果类型的字段和用法见 [协程与并发](tasks.md#任务终态与结果类型)。

## 先读懂返回值

`task<T>` 表示一个异步任务，对它 `co_await` 之后拿到 `T`。网络和文件操作用 `expected<T>` 表示成功或失败：

```cpp
const auto count = co_await stream.read(buffer);
if (!count) {
  faio::log::logger()->error("read failed: {}", count.error().message());
  co_return;
}
// buffer 非空时，*count == 0 表示对端 EOF。
```

各模块表格里说的“完成结果”，就是 `co_await` 之后取得的值。等待器类型不需要显式写出：用 `auto` 接住操作，或直接 `co_await`。

第一次使用建议从 [快速开始](../getting-started.md) 跑通完整程序；错误域、部分传输与异常规则见 [错误与日志](errors.md)。
