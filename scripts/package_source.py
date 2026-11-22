#!/usr/bin/env python3
"""@file package_source.py
@brief 打包发布源码、用户文档与构建配置。
"""
import argparse
import hashlib
import os
from pathlib import Path
import tarfile

def main():
    """@brief 生成源码归档、文件清单与 SHA256 摘要。"""
    parser = argparse.ArgumentParser(description="生成发布源码归档。")
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    excluded = {'.git', '.idea', '.vscode', '.history', '.workbuddy', '.rkdev', '.cache',
                '.clice', '.tmp', '.zed', '.xmake', 'gcm.cache', '_deps', 'CMakeFiles',
                'node_modules', 'dist', 'target', '__pycache__', 'vcpkg_installed',
                '开发过程文档', '阅读笔记'}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    generated = {p.resolve() for p in (args.output,
        args.output.with_suffix(args.output.suffix+'.manifest'),
        args.output.with_suffix(args.output.suffix+'.sha256'))}
    files = []
    with tarfile.open(args.output,'w:gz') as archive:
        for current, directories, filenames in os.walk(repo):
            directory = Path(current)
            directories[:] = sorted(
                name for name in directories
                if name not in excluded
                and name != 'build'
                and not name.startswith('cmake-build-')
                and not (directory == repo and name.startswith('bazel-'))
                and not (directory == repo / 'benchmark' and name == 'result')
            )
            for filename in sorted(filenames):
                path = Path(current) / filename
                if path.resolve() in generated: continue
                relative = path.relative_to(repo)
                if path.name in {'.DS_Store', 'clice.toml', 'CMakeUserPresets.json', 'compile_commands.json'}:
                    continue
                if path.suffix in {'.o', '.obj', '.exe', '.a', '.dylib', '.so', '.pyc', '.pcm', '.ifc', '.gcm'}:
                    continue
                if path.is_symlink() and path.parent == repo and filename.startswith('bazel-'):
                    continue
                archive.add(path, arcname=relative, recursive=False)
                files.append(relative.as_posix())
    args.output.with_suffix(args.output.suffix+'.manifest').write_text('\n'.join(files)+'\n')
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
    args.output.with_suffix(args.output.suffix+'.sha256').write_text(digest+'  '+args.output.name+'\n')
    print(f'{args.output}: {len(files)} files, sha256 {digest}')
if __name__ == '__main__': main()
