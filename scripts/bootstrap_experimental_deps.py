#!/usr/bin/env python3
"""@file bootstrap_experimental_deps.py
@brief 获取并验证锁定版本的实验性依赖。
"""

from __future__ import annotations

import argparse
import errno
import hashlib
import io
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from urllib.error import URLError
from pathlib import Path
from urllib.parse import urlsplit
from urllib.request import urlopen


EXPERIMENT_DEPENDENCIES = ("stdexec", "asio-standalone", "flare")
MAX_ARCHIVE_BYTES = 128 * 1024 * 1024
MAX_SOURCE_BYTES = 128 * 1024 * 1024
MAX_ARCHIVE_MEMBERS = 30_000


class DependencyError(RuntimeError):
    """@brief 依赖与版本清单不匹配。"""


def git(source: Path | None, *arguments: str) -> str:
    """@brief 执行 Git 命令并返回输出。"""
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


def read_lock(path: Path) -> dict:
    """@brief 读取并校验实验性依赖版本清单。"""
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or document.get("schema_version") != 2:
        raise DependencyError(f"Unsupported lock schema in {path}")
    dependency = document.get("stdexec", {})
    if not isinstance(dependency, dict) or not all(isinstance(dependency.get(key), str) and dependency[key]
               for key in ("repository", "tag", "commit")):
        raise DependencyError(f"Incomplete stdexec lock in {path}")
    if re.fullmatch(r"[0-9a-f]{40}", dependency["commit"]) is None:
        raise DependencyError("stdexec lock must contain a complete lowercase Git commit")
    return document


def verify_checkout(source: Path, dependency: dict[str, str]) -> None:
    """@brief 检查源码仓库、版本与工作区状态。"""
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
    """@brief 准备或验证固定版本源码。

    @details 已有源码按清单校验；失败时清理本次创建的暂存目录。
    """
    if source.exists() or source.is_symlink():
        verify_checkout(source, dependency)
        return
    if verify_only:
        raise DependencyError(f"Missing stdexec checkout: {source}; run this script without --verify-only")
    source.parent.mkdir(parents=True, exist_ok=True)
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


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def present(path: Path) -> bool:
    return path.exists() or path.is_symlink()


def path_parts(path: str) -> list[str]:
    if not isinstance(path, str) or any(c in path for c in "\\:\0"):
        raise DependencyError(f"Unsafe archive path: {path!r}")
    parts = path.split("/")
    if any(not part or part.endswith((".", " ")) or part.lower() == ".git" for part in parts):
        raise DependencyError(f"Unsafe archive path: {path!r}")
    return parts


def link_destination(path: str, target: str) -> str:
    parts = path_parts(path)[:-1]
    if not isinstance(target, str) or any(c in target for c in "\\:\0"):
        raise DependencyError(f"Unsafe archive link: {path!r} -> {target!r}")
    for part in target.split("/"):
        if part == ".." and parts:
            parts.pop()
        elif part in {"", ".", ".."}:
            raise DependencyError(f"Unsafe archive link: {path!r} -> {target!r}")
        else:
            parts.append(part)
    if not parts:
        raise DependencyError(f"Unsafe archive link: {path!r} -> {target!r}")
    return "/".join(parts)


def is_https(url: str) -> bool:
    if not isinstance(url, str):
        return False
    parsed = urlsplit(url)
    return parsed.scheme == "https" and bool(parsed.hostname) and parsed.username is None


def check_archive_lock(name: str, dependency: dict) -> None:
    if not isinstance(dependency, dict):
        raise DependencyError(f"Missing archive lock for {name}")
    check_download_lock(name, dependency)
    if not is_https(dependency.get("repository")):
        raise DependencyError(f"Invalid repository in {name} lock")
    for field, size in (("commit", 40), ("sha256", 64), ("tree_sha256", 64)):
        value = dependency.get(field)
        if not isinstance(value, str) or re.fullmatch(f"[0-9a-f]{{{size}}}", value) is None:
            raise DependencyError(f"Invalid {field} in {name} lock")
    for field in ("filename", "strip_prefix"):
        if len(path_parts(dependency.get(field))) != 1:
            raise DependencyError(f"Invalid {field} in {name} lock")
    if not dependency["filename"].endswith(".tar.gz"):
        raise DependencyError(f"Invalid archive extension in {name} lock")
    tag = dependency.get("tag")
    if "tag" not in dependency or (tag is not None and (not isinstance(tag, str) or not tag)):
        raise DependencyError(f"Invalid tag in {name} lock")
    checks = dependency.get("include_checks")
    if not isinstance(checks, list) or not checks:
        raise DependencyError(f"Missing include checks in {name} lock")
    for path in checks:
        path_parts(path)
    links = dependency.get("symlinks", {})
    if not isinstance(links, dict):
        raise DependencyError(f"Invalid symlink map in {name} lock")
    for path, target in links.items():
        link_destination(path, target)


def check_download_lock(name: str, archive: dict) -> None:
    if not isinstance(archive, dict):
        raise DependencyError(f"Missing download lock for {name}")
    if not is_https(archive.get("url")):
        raise DependencyError(f"Invalid URL in {name} lock")
    mirrors = archive.get("mirrors", [])
    if not isinstance(mirrors, list) or not all(is_https(url) for url in mirrors):
        raise DependencyError(f"Invalid mirrors in {name} lock")
    digest = archive.get("sha256")
    if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        raise DependencyError(f"Invalid SHA256 in {name} lock")
    if len(path_parts(archive.get("filename"))) != 1:
        raise DependencyError(f"Invalid filename in {name} lock")


def require_digest(data: bytes, expected: str, label: str) -> None:
    actual = sha256(data)
    if actual != expected:
        raise DependencyError(f"SHA256 mismatch for {label}: expected {expected}, found {actual}")


def read_verified_archive(path: Path, digest: str) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise DependencyError(f"Archive must be a regular file: {path}")
    with path.open("rb") as archive:
        data = archive.read(MAX_ARCHIVE_BYTES + 1)
    if len(data) > MAX_ARCHIVE_BYTES:
        raise DependencyError(f"Archive exceeds bootstrap size limit: {path}")
    require_digest(data, digest, str(path))
    return data


def archive_bytes(archive_dir: Path, dependency: dict, offline: bool) -> bytes:
    path = archive_dir / dependency["filename"]
    if present(path):
        return read_verified_archive(path, dependency["sha256"])
    if offline:
        raise DependencyError(f"Missing offline archive: {path}")

    ## @brief 在同一文件系统中暂存下载内容，仅在校验通过后写入最终位置。
    archive_dir.mkdir(parents=True, exist_ok=True)
    urls = [dependency["url"], *dependency.get("mirrors", [])]
    errors = []
    for url in urls:
        try:
            with urlopen(url, timeout=45) as response:
                if not is_https(response.geturl()):
                    raise DependencyError(f"Archive URL redirected away from HTTPS: {url}")
                data = response.read(MAX_ARCHIVE_BYTES + 1)
            if len(data) > MAX_ARCHIVE_BYTES:
                raise DependencyError(f"Archive exceeds bootstrap size limit: {url}")
            require_digest(data, dependency["sha256"], url)
            break
        except DependencyError:
            raise
        except (URLError, TimeoutError) as error:
            errors.append(f"{url}: {error}")
    else:
        raise DependencyError(f"Unable to fetch {path.name}: {'; '.join(errors)}")
    with tempfile.TemporaryDirectory(prefix=".archive-", dir=archive_dir) as temp:
        staged = Path(temp) / path.name
        staged.write_bytes(data)
        try:
            os.link(staged, path)
        except FileExistsError:
            return read_verified_archive(path, dependency["sha256"])
    return data


def extract_source(data: bytes, source: Path, dependency: dict) -> None:
    prefix = dependency["strip_prefix"]
    approved_links = dependency.get("symlinks", {})
    links = {}
    seen = set()
    total_size = 0
    source.mkdir()
    source = source.resolve()
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        for index, member in enumerate(archive):
            if index == MAX_ARCHIVE_MEMBERS:
                raise DependencyError("Too many members in source archive")
            parts = path_parts(member.name)
            if parts[0] != prefix or (len(parts) == 1 and not member.isdir()):
                raise DependencyError(f"Unexpected archive root: {member.name}")
            relative = "/".join(parts[1:])
            if relative in seen:
                raise DependencyError(f"Duplicate archive member: {member.name}")
            seen.add(relative)
            destination = source.joinpath(*parts[1:])
            if member.isdir():
                destination.mkdir(parents=True, exist_ok=True)
            elif member.issym() and approved_links.get(relative) == member.linkname:
                links[relative] = member.linkname
            elif member.type in (tarfile.REGTYPE, tarfile.AREGTYPE):
                total_size += member.size
                if member.size < 0 or total_size > MAX_SOURCE_BYTES:
                    raise DependencyError(f"Archive contents exceed bootstrap size limit: {member.name}")
                destination.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as content, destination.open("xb") as output:
                    shutil.copyfileobj(content, output)
                destination.chmod(0o755 if member.mode & 0o111 else 0o644)
            else:
                raise DependencyError(f"Unsupported archive member: {member.name}")
    if links != approved_links:
        raise DependencyError("Archive symlinks do not match the lock")
    for path, target in links.items():
        resolved = source / link_destination(path, target)
        if not resolved.exists() or source not in resolved.resolve().parents:
            raise DependencyError(f"Missing or out-of-tree archive link target: {path}")
        (source / path).symlink_to(target, target_is_directory=resolved.is_dir())


def verify_source(source: Path, dependency: dict) -> None:
    if source.is_symlink() or not source.is_dir():
        raise DependencyError(f"Expected a real source directory: {source}")
    source = source.resolve()
    for include in dependency["include_checks"]:
        header = source / include
        if header.is_symlink() or not header.is_file() or source not in header.resolve().parents:
            raise DependencyError(f"Missing locked header: {header}")

    records = []
    for parent, directories, files in os.walk(source, onerror=raise_walk_error):
        for name in [*directories, *files]:
            path = Path(parent) / name
            relative = path.relative_to(source).as_posix()
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode):
                target = os.readlink(path)
                if dependency.get("symlinks", {}).get(relative) != target:
                    raise DependencyError(f"Unexpected source symlink: {path}")
                records.append((relative, "link", target))
            elif stat.S_ISREG(mode):
                with path.open("rb") as stream:
                    digest = hashlib.sha256()
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        digest.update(chunk)
                records.append((relative, "exec" if mode & 0o111 else "file", digest.hexdigest()))
            elif not stat.S_ISDIR(mode):
                raise DependencyError(f"Unexpected source file type: {path}")
    manifest = json.dumps(sorted(records), ensure_ascii=True, separators=(",", ":")).encode()
    require_digest(manifest, dependency["tree_sha256"], str(source))


def raise_walk_error(error: OSError) -> None:
    raise error


def experiment_manifest(dependencies: dict) -> dict:
    manifest_fields = ("repository", "tag", "commit", "url", "filename", "sha256", "tree_sha256", "include_checks")
    return {"schema_version": 1, "dependencies": {
        name: {"path": name, **{field: dependency[field] for field in manifest_fields}}
        for name, dependency in dependencies.items()
    }}


def prepare_flare_archives(archive_dir: Path, document: dict, offline: bool) -> None:
    bundle = document.get("flare_bazel", {})
    archives = bundle.get("archives", [])
    if not isinstance(archives, list) or not archives:
        raise DependencyError("Flare Bazel archives are missing from the lock")
    for archive in archives:
        check_download_lock("Flare Bazel archive", archive)
        archive_bytes(archive_dir, archive, offline)


def bootstrap_experiment(root: Path, archive_dir: Path, document: dict, offline: bool,
                         verify_only: bool, with_flare_bazel: bool) -> None:
    if not root.is_dir():
        raise DependencyError(f"Experiment copy must already exist: {root}")
    third_party = root / "third_party"
    if third_party.is_symlink():
        raise DependencyError(f"Experiment third_party must be a real directory: {third_party}")
    dependencies = {name: document.get(name) for name in EXPERIMENT_DEPENDENCIES}
    for name, dependency in dependencies.items():
        check_archive_lock(name, dependency)
    manifest = experiment_manifest(dependencies)
    manifest_path = third_party / "manifest.json"
    if present(manifest_path):
        if manifest_path.is_symlink() or not manifest_path.is_file() or json.loads(
            manifest_path.read_text(encoding="utf-8")
        ) != manifest:
            raise DependencyError(f"Source manifest differs from the lock: {manifest_path}")
    elif verify_only:
        raise DependencyError(f"Missing experiment manifest: {manifest_path}")

    missing = []
    for name, dependency in dependencies.items():
        source = third_party / name
        if verify_only or (offline and present(source)):
            archive_bytes(archive_dir, dependency, offline=True)
        if present(source):
            verify_source(source, dependency)
        elif verify_only:
            raise DependencyError(f"Missing source directory: {source}")
        else:
            missing.append(name)
    if with_flare_bazel:
        prepare_flare_archives(archive_dir, document, offline or verify_only)
    if verify_only or (not missing and present(manifest_path)):
        return

    third_party.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".bootstrap-", dir=third_party) as temp:
        staging = Path(temp)
        for name in missing:
            dependency = dependencies[name]
            data = archive_bytes(archive_dir, dependency, offline)
            extract_source(data, staging / name, dependency)
            verify_source(staging / name, dependency)
        for name in missing:
            source = third_party / name
            try:
                (staging / name).rename(source)
            except OSError as error:
                if error.errno not in (errno.EEXIST, errno.ENOTEMPTY):
                    raise
                verify_source(source, dependencies[name])
        staged_manifest = staging / "manifest.json"
        staged_manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        subprocess.run([sys.executable, "-m", "json.tool", str(staged_manifest)],
                       stdout=subprocess.DEVNULL, check=True)
        try:
            os.link(staged_manifest, manifest_path)
        except FileExistsError:
            if manifest_path.is_symlink() or json.loads(manifest_path.read_text(encoding="utf-8")) != manifest:
                raise DependencyError(f"Source manifest appeared during bootstrap: {manifest_path}")


def main() -> int:
    """@brief 解析参数并准备实验性依赖。"""
    project_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="获取并验证锁定版本的实验性依赖。")
    parser.add_argument("--workspace-root", "--dependency-root", dest="dependency_root", type=Path,
                        default=project_root.parent / "faio-deps",
                        help="依赖缓存目录，默认为项目相邻的faio-deps")
    parser.add_argument("--verify-only", action="store_true",
                        help="离线校验已有源码")
    parser.add_argument("--experiment-root", type=Path,
                        help="已有独立实验副本，源码写入其 third_party 目录")
    parser.add_argument("--archive-dir", type=Path,
                        help="归档缓存路径，默认为依赖缓存目录下的 archives")
    parser.add_argument("--offline", action="store_true",
                        help="使用本地已锁定归档，在实验副本离线重建")
    parser.add_argument("--with-flare-bazel", action="store_true",
                        help="同时准备或校验 Flare native fiber 的 Bazel distdir 源码归档")
    arguments = parser.parse_args()
    if not arguments.experiment_root and (arguments.archive_dir or arguments.offline or arguments.with_flare_bazel):
        parser.error("--archive-dir, --offline and --with-flare-bazel require --experiment-root")
    try:
        document = read_lock(project_root / "scripts" / "experimental_dependencies.lock.json")
        if arguments.experiment_root:
            root = arguments.experiment_root.resolve()
            archive_dir = arguments.archive_dir or arguments.dependency_root / "archives"
            bootstrap_experiment(root, archive_dir.resolve(), document, arguments.offline,
                                 arguments.verify_only, arguments.with_flare_bazel)
            print(f"Locked experimental sources: {root / 'third_party' / 'manifest.json'}")
            return 0
        dependency = document["stdexec"]
        source = arguments.dependency_root.resolve() / "src" / "stdexec"
        bootstrap(source, dependency, arguments.verify_only)
        print(f"stdexec {dependency['tag']} at {dependency['commit']}: {source}")
        return 0
    except (DependencyError, OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        print(f"experimental dependency error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
