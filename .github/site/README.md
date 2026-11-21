# faio 文档站点

站点由静态首页、mdBook 使用文档和 Doxygen 核心公共 API 查询组成，生成到 `build/site/`。本地与 GitHub Actions 使用同一构建入口。

## 本地构建

工具：Python 3.9+、Node.js/npm、mdBook 0.5.4、Doxygen 1.18.0。macOS 上的步骤：

```sh
brew install node doxygen
cargo install mdbook --version 0.5.4 --locked
python3 .github/site/scripts/docs.py setup
python3 .github/site/scripts/docs.py build
python3 .github/site/scripts/docs.py check
```

本地预览：

```sh
python3 .github/site/scripts/docs.py serve --port 8000
```

打开 `http://127.0.0.1:8000/`。文档、公共头文件或例程修改后会自动重新构建；刷新浏览器查看结果。构建在暂存目录完成并检查链接，通过后替换 `build/site/`。

## 内容目录

| 路径 | 内容 |
| --- | --- |
| `docs/getting-started.md` | 工具链、构建和工程集成 |
| `docs/learn-faio.md` + `docs/tutorial/` | TCP 回显服务贯穿的十章渐进教程 |
| `docs/topics/` + `docs/glossary.md` | 工程专题与术语表 |
| `docs/api/` | 公共 API 介绍与调用约定 |
| `docs/api-reference.md` | API 查询说明 |
| `docs/SUMMARY.md` | 使用文档导航 |
| `开发过程文档/` | 设计说明、开发验证与性能记录，不参与站点构建 |
| `.github/site/index.html`、`assets/` | 首页和静态资源 |
| `.github/site/api/` | Doxygen 配置与 API 首页 |
| `.github/site/scripts/api_reference*.py` | 公共签名提取、中文接口描述和覆盖校验 |
| `.github/site/theme/`、`book.toml` | 文档样式与 mdBook 配置 |
| `.github/site/scripts/docs.py` | 构建、链接检查与预览 |
| `.github/workflows/docs.yml` | 验证及 GitHub Pages 发布 |

API 介绍位于 `/book/api/`，覆盖核心模块、TLS、进程与实验层；核心公开 API 的查询位于 `/api/`。构建从真实头文件生成 XML，按公共导出范围生成声明视图，再输出类型页、函数重载、参数和返回说明。原始 XML 与声明视图位于 `build/api-source/`、`build/api-public/`。

新增公共类型时更新 `api_reference_content.py` 的 `EXPORTS`，公开函数更新 `FUNCTION_GROUPS`。缺少方法、参数或枚举值说明，或者出现 Doxygen 文档警告，都会使构建失败。签名、默认值和重载从源码提取。

Mermaid 与代码高亮资源随站点分发。`node_modules/` 和 `build/` 为生成目录。

## 发布

仓库 GitHub Pages 的 Source 设置为 GitHub Actions。Documentation 工作流在 PR 中构建并检查预览；main 分支构建通过后部署。默认网址为 `https://superlxh02.github.io/faio/`。

fork 后需更新仓库 URL、编辑链接和 404 页的首页链接。站点支持项目子路径及本地预览。
