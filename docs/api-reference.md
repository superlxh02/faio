# API 查询

[打开 API 查询 →](../api/index.html)

通过页面顶部的搜索框查找已收录的核心公开类型或函数，例如 `task`、`spawn`、`config_builder`、`TcpStream` 和 `File`。类型页列出成员与重载；函数详情包含签名、参数、返回值、错误和生命周期约定。

查询范围包括任务、运行时、网络、文件、时间、同步、流操作及错误与日志。TLS、异步进程和实验层分别见[TLS介绍](api/tls.md)、[进程介绍](api/process.md)与[实验性接口介绍](api/experimental.md)。

| 查询方式 | 适用场景 |
| --- | --- |
| 搜索框 | 已知类型或函数名 |
| 模块列表 | 按任务、网络、文件等 API 分组浏览 |
| 类型成员列表 | 查找构造、方法、属性及重载 |
| [API 介绍](api/index.md) | 了解头文件、调用顺序和示例 |

异步接口通过 `co_await` 取得完成结果。读写操作通常返回 `expected<T>`，先检查成功状态，再访问结果或错误；输入缓冲区须存活到操作完成。

## 本地查询

安装文档工具后，在仓库根目录执行：

```sh
python3 .github/site/scripts/docs.py build
python3 .github/site/scripts/docs.py serve
```

浏览器打开 `http://127.0.0.1:8000/api/`。工具安装和站点构建说明见[文档工具](../.github/site/README.md)。生成文件位于 `build/site/api/`。
