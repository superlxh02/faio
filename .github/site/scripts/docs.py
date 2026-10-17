#!/usr/bin/env python3
"""Build and preview the same static site locally and on GitHub Actions."""
import argparse
from functools import partial
from html import escape
from html.parser import HTMLParser
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from threading import Thread
import time
from urllib.parse import unquote, urlsplit

SOURCE = Path(__file__).resolve().parent.parent
ROOT = SOURCE.parent.parent
SITE = ROOT / "build/site"
MERMAID = SOURCE / "node_modules/mermaid/dist/mermaid.min.js"


def run(*args):
    subprocess.run(args, cwd=ROOT, check=True)


def require_tools():
    missing = [name for name in ("mdbook", "npm", "doxygen") if not shutil.which(name)]
    if missing:
        raise RuntimeError("缺少工具：" + ", ".join(missing) + "。请参考 .github/site/README.md 安装。")


def setup():
    require_tools()
    run("npm", "ci", "--prefix", str(SOURCE), "--ignore-scripts", "--no-audit", "--no-fund")
    run("mdbook", "--version")


def highlight_cpp(source):
    pattern = r'("(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|\b(?:co_await|co_return|int|auto|const|using|namespace)\b|\b\d+(?:ms)?\b|#include)'
    tokens = re.split(pattern, source)
    rendered = []
    for index, token in enumerate(tokens):
        if index % 2:
            kind = "comment" if token.startswith("//") else "string" if token[0] in "\"'" else "number" if token[0].isdigit() else "keyword"
            rendered.append(f'<span class="{kind}">{escape(token)}</span>')
        else:
            rendered.append(escape(token))
    return '<pre><code>' + "".join(rendered) + '</code></pre>'


def build():
    require_tools()
    check_public_api()
    if not MERMAID.exists():
        setup()
    output = ROOT / "build/docs-staging"
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True)
    # Override mdBook's bundled highlighter with locally installed languages for
    # C++, shell, PowerShell, CMake and diagrams. Keep mdBook's legacy call API.
    theme = ROOT / "build/docs-theme"
    theme.mkdir(parents=True, exist_ok=True)
    highlighter = SOURCE / "node_modules/@highlightjs/cdn-assets"
    if not highlighter.exists():
        setup()
    pieces = [(highlighter / "highlight.min.js").read_text(encoding="utf-8")]
    pieces.extend((highlighter / f"languages/{language}.min.js").read_text(encoding="utf-8") for language in ("cmake", "powershell"))
    pieces.append("hljs.registerLanguage('mermaid', () => ({contains:[], disableAutodetect:true})); hljs.highlightBlock = element => hljs.highlightElement(element);")
    (theme / "highlight.js").write_text("\n".join(pieces), encoding="utf-8")
    shutil.copyfile(SOURCE / "assets/favicon.svg", theme / "favicon.svg")
    run("mdbook", "build", str(SOURCE), "--dest-dir", str(output / "book"))
    check_public_api(output / "book/api")
    shutil.copytree(SOURCE / "assets", output / "assets", dirs_exist_ok=True)
    source = (ROOT / "examples/quickstart.cpp").read_text(encoding="utf-8")
    excerpt = source[source.index("faio::task<int> delayed_value"):source.index("\nint main()")].strip()
    homepage = (SOURCE / "index.html").read_text(encoding="utf-8").replace("<!-- QUICKSTART_CODE -->", highlight_cpp(excerpt))
    (output / "index.html").write_text(homepage, encoding="utf-8")
    (output / ".nojekyll").touch()
    (output / "404.html").write_text('''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>页面未找到 · faio</title><style>body{background:#090b11;color:#b7c8eb;font:18px/1.8 system-ui;max-width:640px;margin:18vh auto;padding:24px}a{color:#8daeff}</style><h1>404 / 这条路径暂未连接。</h1><p>请检查地址，或使用浏览器返回上一页。</p><p><a href="https://superlxh02.github.io/faio/">返回 faio 首页 →</a></p></html>''', encoding="utf-8")
    shutil.rmtree(output / "book/.obsidian", ignore_errors=True)
    shutil.copyfile(ROOT / "LICENSE", output / "assets/LICENSE.txt")
    shutil.copyfile(SOURCE / "node_modules/mermaid/LICENSE", output / "book/mermaid-LICENSE.txt")
    shutil.copyfile(highlighter / "LICENSE", output / "book/highlight-LICENSE.txt")
    run(sys.executable, str(SOURCE / "scripts/api_reference.py"), "--build")
    check_public_api(output / "api")
    check_links(output)
    previous = ROOT / "build/docs-previous"
    shutil.rmtree(previous, ignore_errors=True)
    if SITE.exists():
        SITE.rename(previous)
    output.rename(SITE)
    shutil.rmtree(previous, ignore_errors=True)
    print(f"\n网站已构建：{SITE}", flush=True)


class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.targets = []
        self.anchors = set()

    def handle_starttag(self, tag, attrs):
        for name, value in attrs:
            if name == "id" or (tag == "a" and name == "name"):
                self.anchors.add(value)
            if value and ((tag in ("a", "link") and name == "href") or (tag in ("script", "img", "iframe") and name == "src")):
                self.targets.append(value)


def check_public_api(directory=None):
    """The API reference is an explicitly curated user-facing surface."""
    pattern = "*.html" if directory is not None else "*.md"
    directory = directory or ROOT / "docs/api"
    for path in directory.glob(pattern):
        source = path.read_text(encoding="utf-8")
        if re.search(r"\bdetail(?:::|/)|/include/faio/detail|api/files\.html", source):
            raise RuntimeError(f"公开 API 文档引用了实现接口：{path.relative_to(ROOT)}")


def check_links(directory=SITE):
    failures = set()
    pages = list(directory.rglob("*.html"))
    if not pages:
        raise RuntimeError("没有找到构建产物，请先执行 build。")
    parsed = {}
    for page in pages:
        parser = Links()
        parser.feed(page.read_text(encoding="utf-8"))
        parsed[page.resolve()] = parser
    for page in pages:
        for target in parsed[page.resolve()].targets:
            parts = urlsplit(target)
            if parts.scheme or parts.netloc:
                continue
            if parts.path.startswith("/"):
                failures.add(f"{page.relative_to(directory)} → {target} (不支持项目子路径)")
                continue
            path = (page.parent / unquote(parts.path)).resolve() if parts.path else page.resolve()
            if path.is_dir():
                path = path / "index.html"
            if not path.is_file() and not (path.is_dir() and (path / "index.html").is_file()):
                failures.add(f"{page.relative_to(directory)} → {target}")
            elif parts.fragment and path in parsed and unquote(parts.fragment) not in parsed[path].anchors:
                failures.add(f"{page.relative_to(directory)} → {target} (锚点不存在)")
    if failures:
        raise RuntimeError("发现失效的站内链接或资源：\n" + "\n".join(sorted(failures)[:60]))
    print(f"已检查 {len(pages)} 个 HTML 页面的站内链接、锚点与资源。", flush=True)


def fingerprint():
    files = []
    for directory in (ROOT / "docs", SOURCE, ROOT / "include", ROOT / "examples"):
        for current, directories, names in os.walk(directory):
            directories[:] = [name for name in directories if name not in ("node_modules", ".obsidian", "__pycache__")]
            files.extend(Path(current) / name for name in names)
    return tuple((str(path), path.stat().st_mtime_ns) for path in sorted(files))


def serve(args):
    build()
    handler = partial(SimpleHTTPRequestHandler, directory=str(SITE))
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    Thread(target=server.serve_forever, daemon=True).start()
    print(f"Local: http://127.0.0.1:{args.port}/", flush=True)
    print("监听文档、页面、头文件和例程变更；构建完成后刷新浏览器。Ctrl+C 退出。", flush=True)
    previous = fingerprint()
    try:
        while True:
            time.sleep(1)
            current = fingerprint()
            if current != previous:
                previous = current
                print("\n检测到修改，重新构建…", flush=True)
                try:
                    build()
                except (RuntimeError, subprocess.CalledProcessError) as error:
                    print(f"构建失败：{error}", file=sys.stderr, flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("setup", "build", "check", "serve"))
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    try:
        if args.command == "setup":
            setup()
        elif args.command == "build":
            build()
        elif args.command == "check":
            check_links()
        else:
            serve(args)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"错误：{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
