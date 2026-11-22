# faio 文档站点

站点由三部分拼起来：GitHub Pages 上的静态首页、mdBook 生成的《faio 使用文档》，以及从真实头文件生成的核心公共 API 查询。三者都构建到 `target/site/`，本地和 GitHub Actions 用同一个入口 `.github/site/scripts/docs.py`，两边的行为一致。

## 本地构建

工具版本要和 CI 对齐：Python 3.9+、Node.js/npm、mdBook 0.5.4、Doxygen 1.18.0，后两者的版本号写在 `.github/workflows/docs.yml` 的 `env` 里。macOS 上先补 node 和 doxygen，mdBook 从 cargo 装：

```sh
brew install node doxygen
cargo install mdbook --version 0.5.4 --locked
python3 .github/site/scripts/docs.py setup
python3 .github/site/scripts/docs.py build
python3 .github/site/scripts/docs.py check
```

`setup` 只需要跑一次。它把 `package.json` 和 lock 文件复制到 `target/docs/`，再用 `npm ci` 装好 Mermaid 和代码高亮要用的资源。`build` 才是完整构建：先确认 `docs/api/` 里没有引用实现层接口，然后在 `target/docs/staging` 下装配整站，mdBook 输出到 `book/`，静态首页把 `examples/quickstart.cpp` 的示例抽出来高亮后填进 `index.html`，另外补上 `404.html`、`.nojekyll` 和许可证文件。装配完成后检查站内链接、锚点和资源，全部通过才把暂存目录换成 `target/site/`，中途失败时旧站点原样留在原地。`check` 单独重跑一遍链接检查，改完文档想快速确认时用。

本地预览：

```sh
python3 .github/site/scripts/docs.py serve --port 8000
```

浏览器打开 `http://127.0.0.1:8000/`。serve 会先构建一次，之后每秒给 `docs/`、`.github/site/`、`include/` 和 `examples/` 取一次指纹，发现改动就重建，刷新页面就能看到结果。某次构建失败不会让服务退出，错误打到 stderr，改好之后会自动继续。

## 内容目录

用文档时的导航在 `docs/SUMMARY.md` 里，构建用到的素材分布在这些位置：

| 路径 | 内容 |
| --- | --- |
| `docs/getting-started.md` | 工具链、主线构建、集成到自己的工程，以及具名模块与 Bazel、xmake 入口 |
| `docs/learn-faio.md` + `docs/tutorial/` | 围绕一个 TCP 回显服务展开的十章渐进教程 |
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

`docs/getting-started.md` 覆盖的范围比第一次构建要宽。主线之外，它还写了怎么用 `import faio;` 消费具名模块，以及 CMake、Bazel、xmake 三个入口各自的预设、配置和验证脚本。

API 文档有两个入口。`/book/api/` 下是手写的介绍，覆盖核心模块、TLS、进程和实验层；`/api/` 下是核心公开 API 的查询页。查询页由 Doxygen 生成 XML 后再装配一遍：先从真实头文件跑出 XML，按 `EXPORTS` 圈定的公共导出范围裁出声明视图，最后生成类型页、函数重载、参数和返回说明。原始 XML 与声明视图分别放在 `target/docs/api-source/` 和 `target/docs/api-public/`。

加了公开类型，就去改 `api_reference_content.py` 里的 `EXPORTS`；加了公开函数，改 `FUNCTION_GROUPS`。方法、参数或枚举值缺说明，或者 Doxygen 报出文档警告，构建都会直接失败。签名、默认值和重载一律从源码提取，不在文档里手抄。

Mermaid 和代码高亮的资源随站点一起分发，页面引用的是本地文件，不走 CDN。npm 依赖、主题副本和中间文件都落在 `target/docs/`，可发布的成品在 `target/site/`。

## 发布

仓库 GitHub Pages 的 Source 要设成 GitHub Actions。Documentation 工作流在 PR 和 push 上都会构建：PR 只做验证，并把整站作为 artifact 上传，方便直接看预览；main 分支构建通过后进入 deploy job 发布。默认网址是 `https://superlxh02.github.io/faio/`。

fork 之后要把仓库地址换掉，涉及 `book.toml` 的 `git-repository-url` 和 `edit-url-template`、`docs_links.py` 里的仓库地址、`docs.py` 生成的 404 页，以及 `index.html` 里几处 GitHub 链接。站内链接都是相对路径，项目子路径部署和本地预览都不需要额外配置。
