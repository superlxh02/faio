#!/usr/bin/env python3
"""Explicitly fetch pinned experimental dependencies; never called by configure.

Existing checkouts are only inspected. A different revision, remote repository,
or a dirty work tree is an error: this script never resets a user's checkout.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


class DependencyError(RuntimeError):
    """A dependency does not match the committed lock file."""


def git(source: Path | None, *arguments: str) -> str:
    command = ["git"]
    if source is not None:
        command.extend(["-C", str(source)])
    command.extend(arguments)
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode:
        raise DependencyError(
            f"Git command failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stderr.strip()}"
        )
    return result.stdout.strip()


def read_lock(path: Path) -> dict[str, str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema_version") != 1:
        raise DependencyError(f"Unsupported lock schema in {path}")
    dependency = document.get("stdexec", {})
    if not all(isinstance(dependency.get(key), str) and dependency[key]
               for key in ("repository", "tag", "commit")):
        raise DependencyError(f"Incomplete stdexec lock in {path}")
    if re.fullmatch(r"[0-9a-f]{40}", dependency["commit"]) is None:
        raise DependencyError("stdexec lock must contain a complete lowercase Git commit")
    return dependency


def verify_checkout(source: Path, dependency: dict[str, str]) -> None:
    if not (source / ".git").exists():
        raise DependencyError(f"{source} is not a Git checkout; no source-directory fallback is allowed")
    checkout_root = Path(git(source, "rev-parse", "--show-toplevel")).resolve()
    if checkout_root != source.resolve():
        raise DependencyError(f"{source} is not the checkout root ({checkout_root})")
    actual_repository = git(source, "remote", "get-url", "origin")
    if actual_repository != dependency["repository"]:
        raise DependencyError(
            f"stdexec origin mismatch: expected {dependency['repository']}, "
            f"found {actual_repository}; checkout was left untouched"
        )
    actual_commit = git(source, "rev-parse", "HEAD")
    if actual_commit != dependency["commit"]:
        raise DependencyError(
            f"stdexec HEAD mismatch: expected {dependency['commit']}, "
            f"found {actual_commit}; checkout was left untouched"
        )
    tag_commit = git(source, "rev-parse", f"refs/tags/{dependency['tag']}^{{commit}}")
    if tag_commit != dependency["commit"]:
        raise DependencyError(
            f"stdexec tag {dependency['tag']} does not resolve to the locked commit; "
            "checkout was left untouched"
        )
    status = git(source, "status", "--porcelain=v1", "--untracked-files=all")
    if status:
        raise DependencyError(f"stdexec work tree is dirty at {source}; checkout was left untouched\n{status}")
    for header in ("stdexec/execution.hpp", "exec/task.hpp"):
        if not (source / "include" / header).is_file():
            raise DependencyError(f"Missing locked stdexec header: include/{header}")


def bootstrap(source: Path, dependency: dict[str, str], verify_only: bool) -> None:
    if source.exists() or source.is_symlink():
        verify_checkout(source, dependency)
        return
    if verify_only:
        raise DependencyError(f"Missing stdexec checkout: {source}; run this script without --verify-only")
    source.parent.mkdir(parents=True, exist_ok=True)
    # Only this private staging directory is removed on failure. Existing cache
    # contents are never reset, deleted, or silently upgraded.
    staging_root = Path(tempfile.mkdtemp(prefix=".stdexec-bootstrap-", dir=source.parent))
    staging_source = staging_root / "stdexec"
    try:
        git(None, "clone", "--no-checkout", "--single-branch", "--depth", "1",
            "--branch", dependency["tag"], dependency["repository"], str(staging_source))
        git(staging_source, "checkout", "--detach", dependency["commit"])
        verify_checkout(staging_source, dependency)
        if source.exists() or source.is_symlink():
            raise DependencyError(f"Dependency destination appeared during bootstrap: {source}")
        staging_source.rename(source)
    finally:
        shutil.rmtree(staging_root)


def main() -> int:
    project_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace-root", "--dependency-root", dest="dependency_root", type=Path,
                        default=project_root.parent / "faio-deps",
                        help="cache root (default: ../faio-deps relative to the project)")
    parser.add_argument("--verify-only", action="store_true",
                        help="verify an existing checkout without network access or changes")
    arguments = parser.parse_args()
    try:
        dependency = read_lock(project_root / "scripts" / "experimental_dependencies.lock.json")
        source = arguments.dependency_root.resolve() / "src" / "stdexec"
        bootstrap(source, dependency, arguments.verify_only)
        print(f"stdexec {dependency['tag']} at {dependency['commit']}: {source}")
        return 0
    except (DependencyError, OSError, ValueError) as error:
        print(f"experimental dependency error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
