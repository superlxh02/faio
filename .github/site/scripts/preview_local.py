#!/usr/bin/env python3
"""Refresh the whole site (mdBook book + homepage) and serve build/site.

Kept in one process so the local preview matches docs.py build output.
Uses the already-installed toolchain and node_modules.
"""
import shutil
import subprocess
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path("/Users/lxh/workspace/cpp/faio")
SOURCE = ROOT / ".github/site"
SITE = ROOT / "build/site"
THEME = ROOT / "build/docs-theme"
sys.path.insert(0, str(SOURCE / "scripts"))

from docs import highlight_cpp  # noqa: E402


def refresh_book():
    # Theme assets consumed by mdBook [output.html].
    THEME.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(SOURCE / "theme/book.css", THEME / "book.css")
    shutil.copyfile(SOURCE / "theme/book.js", THEME / "book.js")
    shutil.copyfile(SOURCE / "assets/favicon.svg", THEME / "favicon.svg")

    # Local highlight.js bundle (same as docs.py build()).
    highlighter = SOURCE / "node_modules/@highlightjs/cdn-assets"
    pieces = [(highlighter / "highlight.min.js").read_text(encoding="utf-8")]
    pieces.extend(
        (highlighter / f"languages/{language}.min.js").read_text(encoding="utf-8")
        for language in ("cmake", "powershell")
    )
    pieces.append(
        "hljs.registerLanguage('mermaid', () => ({contains:[], disableAutodetect:true})); "
        "hljs.highlightBlock = element => hljs.highlightElement(element);"
    )
    (THEME / "highlight.js").write_text("\n".join(pieces), encoding="utf-8")

    # mdBook writes directly into build/site/book.
    subprocess.run(
        ["mdbook", "build", str(SOURCE), "--dest-dir", str(SITE / "book")],
        cwd=str(ROOT),
        check=True,
    )
    shutil.rmtree(SITE / "book/.obsidian", ignore_errors=True)
    print("mdBook book refreshed.", flush=True)


def refresh_homepage():
    quickstart = (ROOT / "examples/quickstart.cpp").read_text(encoding="utf-8")
    excerpt = quickstart[
        quickstart.index("faio::task<int> delayed_value") : quickstart.index("\nint main()")
    ].strip()
    homepage = (SOURCE / "index.html").read_text(encoding="utf-8")
    homepage = homepage.replace("<!-- QUICKSTART_CODE -->", highlight_cpp(excerpt))
    (SITE / "index.html").write_text(homepage, encoding="utf-8")
    for name in ("home.css", "home.js", "favicon.svg"):
        (SITE / "assets" / name).write_bytes((SOURCE / "assets" / name).read_bytes())
    # 装饰图标 / 组件图标子目录，与 docs.py 的 copytree 保持一致
    shutil.copytree(SOURCE / "assets" / "float", SITE / "assets" / "float", dirs_exist_ok=True)
    shutil.copytree(SOURCE / "assets" / "stack", SITE / "assets" / "stack", dirs_exist_ok=True)
    (SITE / ".nojekyll").touch()
    print("Homepage and assets refreshed.", flush=True)


def main():
    refresh_book()
    refresh_homepage()
    handler = partial(SimpleHTTPRequestHandler, directory=str(SITE))
    server = ThreadingHTTPServer(("127.0.0.1", 8000), handler)
    print("Local: http://127.0.0.1:8000/", flush=True)
    print("Serving build/site. Press Ctrl+C to stop.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
