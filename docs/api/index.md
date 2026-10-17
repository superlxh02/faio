# API 参考

<div class="api-eyebrow">PUBLIC API · C++23</div>

[打开完整 Doxygen 接口参考 →](../../api/index.html)

完整参考为每个公开类型与函数提供独立详情，展开重载签名、形参、返回值、错误及生命周期约定。下方卡片直接进入对应模块；本书各章节保留使用指南和例程说明。

<div class="api-grid">
<a class="api-card" href="../../api/group__tasks.html"><span>faio</span><h3>协程与并发 <b>↗</b></h3><p>task、spawn、join、select 和 scope。</p><code>co_await faio::join(...)</code></a>
<a class="api-card" href="../../api/group__runtime.html"><span>faio::runtime</span><h3>运行时与配置 <b>↗</b></h3><p>调度模式、并发容量与运行时生命周期。</p><code>runtime::configure(...)</code></a>
<a class="api-card" href="../../api/group__network.html"><span>faio::net</span><h3>网络 I/O <b>↗</b></h3><p>TCP、UDP、地址解析与连接操作。</p><code>TcpStream · TcpListener</code></a>
<a class="api-card" href="../../api/group__filesystem.html"><span>faio::fs</span><h3>文件与目录 <b>↗</b></h3><p>文件读写、元数据、路径与目录枚举。</p><code>File · ReadDir · OpenOptions</code></a>
<a class="api-card" href="../../api/group__time.html"><span>faio::time</span><h3>时间与超时 <b>↗</b></h3><p>休眠、周期等待与 I/O 截止时间。</p><code>sleep · interval · timeout</code></a>
<a class="api-card" href="../../api/group__sync.html"><span>faio::sync</span><h3>协程同步 <b>↗</b></h3><p>互斥锁、信号量、条件变量与有界通道。</p><code>mutex · semaphore · mpsc</code></a>
<a class="api-card" href="../../api/group__io.html"><span>faio::io</span><h3>缓冲与流操作 <b>↗</b></h3><p>缓冲区所有权、完整传输和读写适配器。</p><code>io_buffer · BufReader</code></a>
<a class="api-card" href="../../api/group__errors.html"><span>faio · faio::log</span><h3>错误与日志 <b>↗</b></h3><p>检查操作结果，读取错误和传输进度。</p><code>expected&lt;T&gt; · Error</code></a>
<a class="api-card" href="experimental.html"><span>faio::experimental</span><h3>实验性接口 <b>↗</b></h3><p>异步入口注解与 Sender/Receiver 互操作。</p><code>async_main · as_sender</code></a>
</div>

## 包含哪个头文件

| 公共头文件 | 适用场景 |
| --- | --- |
| `<faio/faio.hpp>` | 协程、运行时、网络、文件、时间与同步的聚合入口 |
| `<faio/net.hpp>` | 网络类型与地址解析 |
| `<faio/fs.hpp>` | 文件、目录、元数据与路径操作 |
| `<faio/io.hpp>` | 缓冲区、流算法及原生描述符适配 |
| `<faio/log.hpp>` | faio 日志入口 |
| `<faio/experimental/*.h>` | 满足实验层工具链要求时显式包含 |

## 先读懂返回值

`task<T>` 表达一个异步任务；对它执行 `co_await` 后取得 `T`。网络和文件操作通常以 `expected<T>` 表达成功或失败。

```cpp
const auto count = co_await stream.read(buffer);
if (!count) {
  faio::log::logger()->error("read failed: {}", count.error().message());
  co_return;
}
// buffer 非空时，*count == 0 表示对端 EOF。
```

表格中的“完成结果”指 `co_await` 之后取得的值。无需显式声明操作的等待器类型，使用 `auto` 保存操作或直接等待即可。

第一次使用建议从 [快速开始](../getting-started.md) 运行完整程序；错误域、部分传输与异常规则见 [错误与日志](errors.md)。
