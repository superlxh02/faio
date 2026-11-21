#!/usr/bin/env python3
"""@file faio_mingw_compile.py
@brief 编译 MinGW 对象，并将指定的 TLS 初始化符号改为局部符号。
"""
import argparse
from pathlib import Path
import subprocess
import struct
import sys

SYMBOL = '_ZTHN4faio6detail18current_stop_tokenE'


def has_lto_sections(data, big_coff):
    """@brief 检查 COFF 对象是否包含 LTO 数据段。"""
    if big_coff:
        count, symbols, symbol_count = struct.unpack_from('<III', data, 44)
        section_start, symbol_size = 56, 20
    else:
        count = struct.unpack_from('<H', data, 2)[0]
        symbols, symbol_count = struct.unpack_from('<II', data, 8)
        section_start = 20 + struct.unpack_from('<H', data, 16)[0]
        symbol_size = 18
    string_table = symbols + symbol_count * symbol_size
    for index in range(count):
        offset = section_start + index * 40
        if offset + 40 > len(data):
            raise RuntimeError('truncated COFF section table')
        name = data[offset:offset + 8].split(b'\0', 1)[0]
        if name.startswith(b'/') and name[1:].isdigit():
            beginning = string_table + int(name[1:])
            ending = data.find(b'\0', beginning)
            name = data[beginning:ending] if ending >= beginning else b''
        if name.startswith(b'.gnu.lto'):
            return True
    return False


def main():
    """@brief 执行编译命令，并处理非 LTO COFF 对象中精确匹配的符号。

    @details 输入为普通 COFF 对象时，将指定的 TLS 初始化符号改为局部符号；其他符号及链接产物保持原状。
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--objcopy', required=True)
    parser.add_argument('--nm', required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('compiler command required')
    if '-c' in command and any(option == '-flto' or option.startswith('-flto=') for option in command):
        print('faio: the MinGW GCC TLS compiler launcher requires non-LTO objects; '
              'disable interprocedural optimization/-flto for this target.', file=sys.stderr)
        return 1
    completed = subprocess.run(command)
    if completed.returncode:
        return completed.returncode
    if '-c' not in command or '-fsyntax-only' in command:
        return 0
    output = None
    for index, option in enumerate(command):
        if option == '-o' and index + 1 < len(command):
            output = Path(command[index + 1])
        elif option.startswith('-o') and len(option) > 2:
            output = Path(option[2:])
    if output is None or output.suffix.lower() not in ('.obj', '.o') or not output.is_file():
        return 0
    with output.open('rb') as source:
        header = source.read(8)
        normal_coff = header[:2] in (b'\x64\x86', b'\x4c\x01')
        big_coff = header[:4] == b'\x00\x00\xff\xff' and header[6:8] in (b'\x64\x86', b'\x4c\x01')
        if not normal_coff and not big_coff:
            return 0
    if has_lto_sections(output.read_bytes(), big_coff):
        print('faio: MinGW GCC LTO sections detected; the TLS workaround requires non-LTO objects.', file=sys.stderr)
        return 1
    symbols = subprocess.run([args.nm, '--defined-only', '--extern-only', '--format=posix', str(output)],
                             capture_output=True, text=True, check=True)
    names = {line.split()[0] for line in symbols.stdout.splitlines() if line.strip()}
    if any(name.startswith('__gnu_lto') for name in names) or 'plugin needed' in symbols.stderr.lower():
        print('faio: MinGW GCC LTO object detected; the TLS workaround requires non-LTO objects.', file=sys.stderr)
        return 1
    if SYMBOL not in names:
        return 0
    subprocess.run([args.objcopy, '--localize-symbol=' + SYMBOL, str(output)], check=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
