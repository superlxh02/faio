#!/usr/bin/env python3
"""安装包消费验证：每个公开头独立编译，并将两个翻译单元链接到同一程序。"""
import argparse
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess


def run(argv, cwd):
    subprocess.run(argv, cwd=cwd, check=True, text=True)


def header_applicable(relative, system, native_uring):
    """Classify by implementation directory, keeping platform dispatchers applicable."""
    if '/platform/windows/' in relative and system != 'Windows':
        return False
    if '/platform/unix/' in relative and system == 'Windows':
        return False
    if '/platform/linux/' in relative and system != 'Linux':
        return False
    if '/platform/macos/' in relative:
        # Networking's macOS implementation also supplies the BSD socket policy.
        bsd_network = system == 'FreeBSD' and relative.startswith('detail/net/platform/macos/')
        if system != 'Darwin' and not bsd_network:
            return False
    if relative.startswith('detail/net/unix_domain') and system == 'Windows':
        return False
    for backend, systems in {
        'epoll': {'Linux'}, 'kqueue': {'Darwin', 'FreeBSD'},
        'uring': {'Linux'} if native_uring else set(), 'iocp': {'Windows'},
    }.items():
        if relative.startswith(f'detail/io/backend/{backend}/') and system not in systems:
            return False
    return True


def validate_module_layout(faio_root):
    allowed = {'faio.hpp', 'io.hpp', 'fs.hpp', 'net.hpp', 'log.hpp', 'detail', 'experimental'}
    unexpected = sorted(p.name for p in faio_root.iterdir() if p.name not in allowed)
    if unexpected:
        raise RuntimeError(f'Public entry-point layout violation: {unexpected}')
    io_root = faio_root / 'detail/io'
    actual = {p.name for p in io_root.iterdir() if p.is_file()}
    expected = {'io.hpp', 'engine.hpp', 'traits.hpp'}
    if actual != expected:
        raise RuntimeError(f'IO entry-point layout violation: expected {sorted(expected)}, got {sorted(actual)}')
    for legacy in ('backends', 'util', 'reactor', 'uring'):
        if (io_root / legacy).exists():
            raise RuntimeError(f'Legacy io/{legacy} must not be installed')


def validate_leaf_dependencies(faio_root):
    """Generic contracts may describe requests, but must not select an implementation."""
    leaves = sorted((faio_root / 'detail/io/protocol').glob('*.hpp'))
    leaves += sorted((faio_root / 'detail/io/traits').glob('*.hpp'))
    leaves += [faio_root / f'detail/io/base/{name}.hpp'
               for name in ('backend', 'reactor', 'readiness_adapter')]
    includes = re.compile(r'^\s*#\s*include\s*[<"](faio/[^>"\n]+)[>"]', re.MULTILINE)
    selected = {'detail/io/platform/backend.hpp', 'detail/io/platform/reactor.hpp'}
    for leaf in leaves:
        pending = [(leaf, [leaf.relative_to(faio_root).as_posix()])]
        visited = set()
        while pending:
            header, chain = pending.pop()
            if header in visited:
                continue
            visited.add(header)
            relative = header.relative_to(faio_root).as_posix()
            if relative in selected or relative.startswith('detail/io/backend/'):
                raise RuntimeError('Generic IO contract imports a selected backend: ' + ' -> '.join(chain))
            if not header.is_file():
                raise RuntimeError('Missing installed dependency: ' + ' -> '.join(chain))
            for dependency in includes.findall(header.read_text(encoding='utf-8')):
                target = dependency.removeprefix('faio/')
                pending.append((faio_root / target, chain + [target]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--compiler', help='Override consumer C++ compiler')
    parser.add_argument('--jobs', type=int, default=4, help='Parallel consumer build jobs')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be >= 1')
    repo = Path(__file__).resolve().parents[1]
    build = args.build.resolve()
    work = build / 'installed-consumer'
    source = work / 'source'
    prefix = work / 'prefix'
    source.mkdir(parents=True, exist_ok=True)
    # 安装测试从新的生成prefix开始，避免已经移除的头文件被旧安装残留掩盖。
    if prefix.is_symlink():
        raise RuntimeError('installed-consumer/prefix must be a generated directory, not a symlink')
    if prefix.exists():
        shutil.rmtree(prefix)
    run(['cmake', '--install', str(build), '--prefix', str(prefix)], repo)
    # 验证本平台全部适用的安装头；平台专用后端不在别的平台直接导入。
    faio_root = prefix / 'include/faio'
    validate_module_layout(faio_root)
    validate_leaf_dependencies(faio_root)
    experimental_root = faio_root / 'experimental'
    if experimental_root.exists():
        expected_experimental = {'runtime_options.h', 'runtime.h', 'reflection.h', 'async_main.h', 'execution.h'}
        actual_experimental = {p.name for p in experimental_root.iterdir()}
        if actual_experimental != expected_experimental:
            raise RuntimeError(f'Experimental public header layout violation: {sorted(actual_experimental)}')
    cache = (build / 'CMakeCache.txt').read_text().splitlines()
    native_uring = any(line == 'FAIO_ENABLE_IO_URING:BOOL=ON' for line in cache)
    system = platform.system()
    excluded = []
    experimental_deferred = []

    def applicable(path):
        relative = path.relative_to(faio_root).as_posix()
        # These C++26 capabilities are verified separately by
        # tests/experimental/installed_consumer.cmake; this script stays C++23.
        if relative.startswith(('experimental/', 'detail/experimental/')):
            experimental_deferred.append(relative)
            return False
        invalid = not header_applicable(relative, system, native_uring)
        if invalid:
            excluded.append(relative)
        return not invalid

    headers = sorted(p.relative_to(prefix / 'include').as_posix()
                     for p in faio_root.rglob('*') if p.is_file() and p.suffix in {'.hpp', '.h'} and applicable(p))
    if not headers:
        raise RuntimeError('no installed public headers')
    for index, header in enumerate(headers):
        (source / f'header_{index}.cpp').write_text(f'#include <{header}>\n', encoding='utf-8')
    (source / 'first.cpp').write_text(
        '#include <faio/faio.hpp>\nint second(const char*);\nint main(int argc, char** argv) { return second(argc > 1 ? argv[1] : "") == 42 ? 0 : 1; }\n',
        encoding='utf-8')
    (source / 'second.cpp').write_text('''#include <faio/faio.hpp>
#include <string_view>
faio::task<int> value() { co_return 42; }
int second(const char* selected) {
    auto builder = faio::ConfigBuilder{}.set_num_workers(1);
#if defined(__linux__)
    if (std::string_view{selected} == "epoll") builder.set_io_backend(faio::runtime::io_backend::IO_EPOLL);
    if (std::string_view{selected} == "uring") builder.set_io_backend(faio::runtime::io_backend::IO_URING);
#else
    (void)selected;
#endif
    faio::runtime::configure(builder.build());
    auto result = faio::block_on(value());
    faio::runtime::shutdown();
    return result;
}
''', encoding='utf-8')
    cmake = '''cmake_minimum_required(VERSION 3.20)
project(faio_installed_consumer LANGUAGES CXX)
find_package(faio CONFIG REQUIRED)
add_library(headers OBJECT ''' + ' '.join(f'header_{i}.cpp' for i in range(len(headers))) + ''')
target_link_libraries(headers PRIVATE faio::faio)
add_executable(consumer first.cpp second.cpp)
target_link_libraries(consumer PRIVATE faio::faio)
'''
    (source / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
    compiler = args.compiler
    if not compiler:
        for line in cache:
            if line.startswith('CMAKE_CXX_COMPILER:'):
                compiler = line.split('=', 1)[1];
                break
    argv = ['cmake', '-S', str(source), '-B', str(work / 'build'), '-G', 'Ninja',
            '-DCMAKE_PREFIX_PATH=' + str(prefix) + (';/opt/homebrew' if system == 'Darwin' else ''),
            # Microsoft STL 的迭代器 ABI/CRT 必须与刚安装的依赖库配置相同。
            '-DCMAKE_BUILD_TYPE=' + next((line.split('=', 1)[1] for line in cache
                                         if line.startswith('CMAKE_BUILD_TYPE:')), 'Release')]
    if compiler:
        argv.append('-DCMAKE_CXX_COMPILER=' + compiler)
    run(argv, repo)
    run(['cmake', '--build', str(work / 'build'), '--parallel', str(args.jobs)], repo)
    run([str(work / 'build' / ('consumer.exe' if system == 'Windows' else 'consumer'))], repo)
    if system == 'Linux':
        run([str(work / 'build/consumer'), 'uring' if native_uring else 'epoll'], repo)
    (work / 'verified_headers.txt').write_text('\n'.join(headers) + '\n', encoding='utf-8')
    (work / 'excluded_platform_headers.txt').write_text('\n'.join(excluded) + '\n', encoding='utf-8')
    (work / 'deferred_experimental_headers.txt').write_text('\n'.join(experimental_deferred) + '\n', encoding='utf-8')
    print(
        f'PASS: {len(headers)} self-contained applicable headers, installed package, two translation units; native_uring={native_uring}')


if __name__ == '__main__':
    main()
