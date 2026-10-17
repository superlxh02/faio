# 文档维护与部署

站点使用原生 HTML 首页、mdBook 指南与 Doxygen 公共接口参考，由同一命令构建，输出可直接作为 GitHub Pages artifact 发布。

## 本地运行

准备 Python 3.9+、Node.js 22+、mdBook 0.5.4 和 Doxygen 1.18.0：

```sh
python3 .github/site/scripts/docs.py setup
python3 .github/site/scripts/docs.py serve
```

打开 `http://127.0.0.1:8000/`。源码变更会触发重新构建，完成后刷新页面。单次构建使用 `python3 .github/site/scripts/docs.py build`，单独检查内部链接使用 `python3 .github/site/scripts/docs.py check`。

正文统一维护在 `docs/`；官网资源与构建工具统一维护在 `.github/site/`。生成内容写入 `build/site/`。

## 修改入口

- 首页：编辑 `.github/site/index.html` 与 `.github/site/assets/`。
- 文档：编辑 `docs/` 中的 Markdown，通过 `docs/SUMMARY.md` 调整目录。
- API 使用指南：编辑 `docs/api/`，明确完成结果、错误与生命周期约束。
- API 逐项参考：通过 `.github/site/scripts/api_reference_content.py` 维护公开导出与功能、形参、返回值及约束说明；`.github/site/scripts/api_reference.py` 从真实头文件读取签名并调用 Doxygen。
- API 范围：只发布明确导出的公共名称；别名的继承方法归到公共类型下。原始源码 XML 保留在构建目录，内部类与源码索引不发布。
- API 校验：缺少方法、参数、枚举值说明或 Doxygen 产生警告会使构建失败；生成后再核对全部函数的参数与返回说明。
- 例程：修改 `examples/*.cpp`；文档通过 include 自动引用，不复制源码。
- 样式：`.github/site/theme/` 为文档与 API 提供统一视觉。

现有模块文档保持原路径，书内相互链接仍可阅读；指向仓库其他文件的相对链接由预处理器转换到 GitHub。CI 可通过 `FAIO_SOURCE_REF` 将源码链接固定到本次构建提交。

## CI 与发布

`.github/workflows/docs.yml` 在 PR 中构建与检查，在 `main` 构建后部署到 GitHub Pages。首次需要仓库管理员在 **Settings → Pages → Source** 选择 **GitHub Actions**。

部署工作流使用 GitHub 官方 Pages artifact / deployment actions，不维护生成内容分支。站点内部使用相对路径，适配项目站点的 `/faio/` 前缀。

完整工具安装、目录说明、产物与发布步骤见 [网站维护 README](../.github/site/README.md)。
