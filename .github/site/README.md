# faio 网站与文档

原生 HTML / CSS / JavaScript 首页、mdBook 指南与 Doxygen 公共 API 参考，全部输出到 `build/site/`。本地和 GitHub Actions 使用相同构建入口。

## 本地环境

依赖：Python 3.9+、Node.js 22+ / npm、mdBook 0.5.4、Doxygen 1.18.0。macOS 可执行：

```sh
brew install node doxygen
cargo install mdbook --version 0.5.4 --locked
python3 .github/site/scripts/docs.py setup
python3 .github/site/scripts/docs.py serve
```

已有匹配版本的工具不必重复安装。如果没有 Rust / Cargo，也可以使用 mdBook 官方 release 的平台二进制。

打开 `http://127.0.0.1:8000/`。修改文档、页面、头文件或例程后会重新构建，刷新浏览器查看结果。切换端口：

```sh
python3 .github/site/scripts/docs.py serve --port 8001
```

单次构建和链接检查：

```sh
python3 .github/site/scripts/docs.py build
python3 .github/site/scripts/docs.py check
```

构建时在暂存目录生成并检查页面，通过后替换 `build/site/`。失败时本地预览保留上一版页面。Mermaid 和代码高亮脚本随站点分发，不依赖 CDN。

## 目录组织

文档正文保留在仓库的 `docs/`，与库源码一起修改和审阅。官网页面、主题、mdBook/Doxygen 配置及构建脚本集中在 `.github/site/`，发布工作流位于 `.github/workflows/docs.yml`。依赖位于 `.github/site/node_modules/`，生成内容位于 `build/`，两者均不提交。

```text
faio/
├── include/faio/               # 库源码，API 签名来源
├── examples/                   # 可编译例程，文档直接引用
├── docs/                       # 唯一的指南、例程说明、模块讲解正文
└── .github/
    ├── workflows/docs.yml      # 验证、打包与 Pages 部署
    └── site/
        ├── index.html          # 官网首页
        ├── assets/             # 首页资源
        ├── theme/              # mdBook 与 Doxygen 样式
        ├── api/                # Doxygen 配置与参考首页
        ├── scripts/            # 构建、API 生成与链接处理
        ├── book.toml           # mdBook 配置，读取 ../../docs
        └── package*.json       # 本地前端依赖及版本锁定
```

[fmt](https://github.com/fmtlib/fmt) 和 [Asio](https://github.com/boostorg/asio) 都在源码仓库的 `doc/` 维护文档；[spdlog](https://github.com/gabime/spdlog#documentation) 使用 GitHub Wiki。本项目保留正文与代码同仓库，方便接口与文档在同一次变更中审阅；`.github/site/` 是为收拢站点工具而采用的项目目录约定，GitHub Pages 只接收 Actions 生成的 artifact。

## 内容与维护

| 路径 | 用途 |
| --- | --- |
| `.github/site/index.html`、`.github/site/assets/` | 首页、品牌资产与交互 |
| `.github/site/book.toml`、`docs/SUMMARY.md` | mdBook 配置与阅读目录 |
| `docs/api/` | API 使用指南与 Doxygen 模块入口 |
| `.github/site/api/` | 两遍 Doxygen 配置与参考首页 |
| `.github/site/scripts/api_reference*.py` | 公共导出范围、逐项说明、签名提取与覆盖检查 |
| `docs/examples/` | 官方例程的运行说明与源码引用 |
| `docs/*.md` | 入门、平台说明和原有模块讲解 |
| `.github/site/theme/` | 文档样式与阅读交互 |
| `.github/site/scripts/docs.py` | 构建、检查、静态服务器与文件监听 |
| `.github/site/scripts/docs_links.py` | 将书内仓库源码引用转换为 GitHub 链接 |
| `.github/workflows/docs.yml` | PR 验证与 GitHub Pages 部署 |

API 使用指南位于 `/book/api/`，逐项 Doxygen 参考位于 `/api/`。首页与文档顶部的 API 导航直接打开 Doxygen。

API 构建分两步：先从真实头文件生成仅供构建使用的 XML，再依据 `.github/site/scripts/api_reference_content.py` 的公共导出范围生成声明视图，交给 Doxygen 输出类型页、函数重载、参数及返回说明。公开别名的继承方法归到公共类型；内部控制块、等待协议、源码和文件树不发布。原始 XML 与声明视图仅保存在 `build/api-source/`、`build/api-public/`。

新增公共类型时维护 `EXPORTS`，新增公开函数时维护 `FUNCTION_GROUPS`；函数和参数的说明在同文件维护。签名、参数类型、默认值与重载来自源码 XML。已选类型新增未说明的方法、参数、枚举值，或第二遍 Doxygen 产生文档警告时，构建会失败。构建还核对生成 XML 中每个函数的参数与返回说明，检查生成页面没有内部命名空间。周期对象通过 `auto` 使用，单独的参考页从实际方法声明生成，避免为返回对象虚构公共类型名。

原有实现解析位于“模块设计”，与 API 参考分开维护。例程页面使用 mdBook include 直接读取仓库 `.cpp`；首页的协程片段取自 `examples/quickstart.cpp`。

不要编辑 `build/site/`，它是可以重新生成的产物。

## GitHub Pages

1. 提交变更。
2. 在仓库 **Settings → Pages → Build and deployment → Source** 选择 **GitHub Actions**。
3. 推送到 `main`，或在 Actions 手动运行 `Documentation`。

PR 只构建与检查，保留可下载的 `documentation-preview`；`main` 构建成功后通过官方 Pages Actions 发布，不需要 `gh-pages` 分支。

CI 下载固定版本的 mdBook 与 Doxygen，并校验 Doxygen 官方 SHA-256；构建及链接检查通过后才发布。

默认地址为 `https://superlxh02.github.io/faio/`。相对链接支持 `/faio/` 子路径和本地根目录。fork 后应更新仓库 URL、编辑链接和 404 页的首页链接。本地预览不代表远端已经发布。
