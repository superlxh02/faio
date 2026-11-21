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
                'node_modules', 'target', '__pycache__', 'vcpkg_installed',
                '开发过程文档', '阅读笔记'}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    generated = {p.resolve() for p in (args.output,
        args.output.with_suffix(args.output.suffix+'.manifest'),
        args.output.with_suffix(args.output.suffix+'.sha256'))}
    files = []
    with tarfile.open(args.output,'w:gz') as archive:
        for current, directories, filenames in os.walk(repo):
            directories[:] = sorted(d for d in directories if d not in excluded and d != 'build' and not d.startswith('cmake-build-') and not (Path(current) == repo / 'benchmark' and d == 'result'))
            for filename in sorted(filenames):
                path = Path(current) / filename
                if path.resolve() in generated: continue
                relative = path.relative_to(repo)
                if path.name == '.DS_Store': continue
                if path.suffix in {'.o','.obj','.exe','.a','.dylib','.so','.pyc'}: continue
                archive.add(path, arcname=relative, recursive=False)
                files.append(relative.as_posix())
    args.output.with_suffix(args.output.suffix+'.manifest').write_text('\n'.join(files)+'\n')
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
    args.output.with_suffix(args.output.suffix+'.sha256').write_text(digest+'  '+args.output.name+'\n')
    print(f'{args.output}: {len(files)} files, sha256 {digest}')
if __name__ == '__main__': main()
