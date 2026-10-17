#!/usr/bin/env python3
"""mdBook preprocessor: keep repository source links valid on GitHub Pages."""
import json
import os
from pathlib import Path
import re
import sys
from urllib.parse import quote, urlsplit


def main():
    sys.stdin.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) > 1 and sys.argv[1] == "supports":
        return 0 if sys.argv[2] == "html" else 1
    context, book = json.load(sys.stdin)
    root = Path(__file__).resolve().parents[3]
    source = (Path(context["root"]) / context["config"]["book"]["src"]).resolve()
    ref = quote(os.environ.get("FAIO_SOURCE_REF", "main"), safe="")
    repository = "https://github.com/superlxh02/faio"

    def visit(items):
        for item in items:
            if not isinstance(item, dict):
                continue
            chapter = item.get("Chapter")
            if chapter is None:
                continue
            origin = source / (chapter.get("source_path") or chapter["path"])

            def replace(match):
                destination = match.group(2)
                parts = urlsplit(destination)
                if parts.scheme or parts.netloc or not parts.path or parts.path.startswith("/"):
                    return match.group(0)
                target = (origin.parent / parts.path).resolve()
                try:
                    relative = target.relative_to(root)
                except ValueError:
                    return match.group(0)
                # mdBook handles chapter URLs. Links outside docs point to the source tree.
                if relative.parts[0] == "docs" or not target.exists():
                    return match.group(0)
                kind = "tree" if target.is_dir() else "blob"
                url = f"{repository}/{kind}/{ref}/{quote(relative.as_posix())}"
                if parts.fragment:
                    url += "#" + parts.fragment
                return match.group(1) + url + match.group(3)

            # Ignore fenced code: C++ examples can themselves contain Markdown-like text.
            segments = re.split(r"(^\s*```[^\n]*\n.*?^\s*```\s*$)", chapter["content"], flags=re.M | re.S)
            chapter["content"] = "".join(
                segment if index % 2 else re.sub(r"(\]\()([^\s)]+)(\))", replace, segment)
                for index, segment in enumerate(segments)
            )
            visit(chapter["sub_items"])

    visit(book.get("items", book.get("sections", [])))
    json.dump(book, sys.stdout, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
