#!/usr/bin/env python3
"""Preview the complete documentation site with the same build used by GitHub Pages."""
from pathlib import Path
import sys

sys.pycache_prefix = str(Path(__file__).resolve().parents[3] / "target/docs/pycache")
from docs import serve


if __name__ == "__main__":
    serve(8000)
