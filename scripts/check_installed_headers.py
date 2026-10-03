#!/usr/bin/env python3
"""安装包消费验证：每个公开头独立编译，并将两个翻译单元链接到同一程序。"""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess


def run(argv, cwd):
    subprocess.run(argv, cwd=cwd, check=True, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--compiler', help='Override consumer C++ compiler')
    args = parser.parse_args()
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
    unexpected = sorted(p.name for p in faio_root.iterdir() if p.name not in {'faio.hpp', 'log.hpp', 'detail'})
    if unexpected:
        raise RuntimeError(f'§5 entry-point layout violation: {unexpected}')
    cache = (build / 'CMakeCache.txt').read_text().splitlines()
    native_uring = any(line == 'FAIO_ENABLE_IO_URING:BOOL=ON' for line in cache)
    legacy_uring = faio_root / 'detail/io/uring'
    if legacy_uring.exists():
        raise RuntimeError('Legacy io/uring must not be installed; all implementations belong to io/backends')
    system = platform.system()
    excluded = []

    def applicable(path):
        relative = path.relative_to(faio_root).as_posix()
        invalid = (('/platform/windows_' in relative and system != 'Windows') or
                   ('/backends/epoll/' in relative and system != 'Linux') or
                   ('/backends/kqueue/' in relative and system not in {'Darwin', 'FreeBSD'}) or
                   ('/backends/uring/' in relative and (system != 'Linux' or not native_uring)) or
                   ('/backends/iocp/' in relative and system != 'Windows'))
        if invalid:
            excluded.append(relative)
        return not invalid

    headers = sorted(p.relative_to(prefix / 'include').as_posix()
                     for p in faio_root.rglob('*.hpp') if applicable(p))
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
            '-DCMAKE_PREFIX_PATH=' + str(prefix) + (';/opt/homebrew' if os.uname().sysname == 'Darwin' else '')]
    if compiler:
        argv.append('-DCMAKE_CXX_COMPILER=' + compiler)
    run(argv, repo)
    run(['cmake', '--build', str(work / 'build'), '-j4'], repo)
    run([str(work / 'build/consumer')], repo)
    if system == 'Linux':
        run([str(work / 'build/consumer'), 'epoll'], repo)
        if native_uring:
            run([str(work / 'build/consumer'), 'uring'], repo)
    (work / 'verified_headers.txt').write_text('\n'.join(headers) + '\n', encoding='utf-8')
    (work / 'excluded_platform_headers.txt').write_text('\n'.join(excluded) + '\n', encoding='utf-8')
    print(
        f'PASS: {len(headers)} self-contained applicable headers, installed package, two translation units; native_uring={native_uring}')


if __name__ == '__main__':
    main()
