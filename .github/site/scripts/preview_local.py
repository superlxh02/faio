#!/usr/bin/env python3
"""Refresh the whole site (mdBook book + homepage) and serve target/site.

Kept in one process so the local preview matches docs.py build output.
Uses the already-installed toolchain and node_modules.
"""
import shutil
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import sys
from pathlib import Path

sys.pycache_prefix = str(Path(__file__).resolve().parents[3] / "target/docs/pycache")
from docs import ROOT, SITE, SOURCE, build_book, highlight_cpp


def refresh_book():
    build_book(SITE / "book")
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
    (SITE / "assets").mkdir(parents=True, exist_ok=True)
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
    print("Serving target/site. Press Ctrl+C to stop.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
