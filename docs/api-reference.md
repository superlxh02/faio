# API 查询

[打开 API 查询 →](../api/index.html)

在页面顶部输入类型、函数或成员名即可查询。例如，搜索 `task_group` 查看任务组的调用签名，搜索 `TlsConnector` 或 `Command` 查看 TLS 握手或子进程接口。

查询页按任务、运行时、网络、文件、时间、同步、流操作等功能分组，也收录可选组件。要了解启用条件和调用示例，请参阅 [TLS 介绍](api/tls.md)、[进程介绍](api/process.md)和[实验性接口介绍](api/experimental.md)。

| 查询方式 | 适用场景 |
| --- | --- |
| 搜索框 | 已知类型或函数名 |
| 模块列表 | 按任务、网络、文件等 API 分组浏览 |
| [模块与头文件索引](../api/public_surface.html) | 按 `import` 或 `#include` 入口查找接口 |
| 类型成员列表 | 查找构造、方法、属性及重载 |
| [API 介绍](api/index.md) | 了解头文件、调用顺序和示例 |

异步接口通过 `co_await` 取得完成结果。读写操作通常返回 `expected<T>`，先检查成功状态，再访问结果或错误；输入缓冲区须存活到操作完成。

## 本地查询

安装文档工具后，在仓库根目录执行：

```sh
python3 .github/site/scripts/docs.py serve
```

浏览器打开 `http://127.0.0.1:8000/api/`。生成文件位于 `target/site/api/`。工具安装和构建说明见[文档工具](../.github/site/README.md)。
