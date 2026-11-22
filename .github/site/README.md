# faio 文档站点

站点包含静态首页、mdBook 生成的《faio 使用文档》和 Doxygen 生成的公共 API 查询。本地和 GitHub Actions 都通过 `.github/site/scripts/docs.py` 构建，输出位于 `target/site/`。

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

浏览器打开 `http://127.0.0.1:8000/`。预览服务先构建整站，随后监听 `docs/`、`.github/site/`、`include/`、`cxx-module/` 和 `examples/`。源码或模块导出发生变化时，API 查询也会重建。构建出错时服务继续提供上一次成功的版本。

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

API 文档有两个入口。`/book/api/` 提供使用指南和示例，`/api/` 提供 Doxygen 查询。查询覆盖基础库、TLS、异步进程与实验组件。生成器读取 `cxx-module/` 中的模块导出，从头文件的 Doxygen XML 提取声明。

修改模块导出或公开签名后，运行 `python3 .github/site/scripts/docs.py build` 即可更新查询页和搜索索引。已有接口的中文详解在 `api_reference_content.py` 的 `EXPORTS` 和 `FUNCTION_GROUPS` 中维护。生成的声明、XML 和覆盖清单位于 `target/docs/api-public/`。

Mermaid 和代码高亮的资源随站点一起分发，页面引用的是本地文件，不走 CDN。npm 依赖、主题副本和中间文件都落在 `target/docs/`，可发布的成品在 `target/site/`。

## 发布

仓库 GitHub Pages 的 Source 要设成 GitHub Actions。Documentation 工作流在 PR 和 push 上都会构建：PR 只做验证，并把整站作为 artifact 上传，方便直接看预览；main 分支构建通过后进入 deploy job 发布。默认网址是 `https://superlxh02.github.io/faio/`。

fork 之后要把仓库地址换掉，涉及 `book.toml` 的 `git-repository-url` 和 `edit-url-template`、`docs_links.py` 里的仓库地址、`docs.py` 生成的 404 页，以及 `index.html` 里几处 GitHub 链接。站内链接都是相对路径，项目子路径部署和本地预览都不需要额外配置。
