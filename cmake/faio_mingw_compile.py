#!/usr/bin/env python3
"""修复 MinGW GCC 的单个 inline TLS initializer COFF 符号，不放宽链接诊断。

GCC 将当前停止令牌的初始化 alias 错误导出为强符号。该 alias 只供当前
翻译单元的 TLS wrapper 使用，局部化后每个 TU 保留自身的初始化函数；
inline TLS 实体及一次性 guard 继续由正常 COMDAT 规则合并。
"""
import argparse
from pathlib import Path
import subprocess
import struct
import sys

SYMBOL = '_ZTHN4faio6detail18current_stop_tokenE'


def has_lto_sections(data, big_coff):
    """读取 COFF section 名称，fat LTO 的优化数据也必须明确拒绝。"""
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--objcopy', required=True)
    parser.add_argument('--nm', required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('compiler command required')
    if '-c' in command and any(option == '-flto' or option.startswith('-flto=') for option in command):
        # slim LTO 对象可能只有 __gnu_lto_slim，普通 nm 看不到后续会再生成的 alias。
        print('faio: the MinGW GCC TLS compiler launcher requires non-LTO objects; '
              'disable interprocedural optimization/-flto for this target.', file=sys.stderr)
        return 1
    # 用户原有的 ccache/sccache launcher 仍作为完整命令的一部分执行。
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
    # 只处理 Windows COFF 对象；预编译头、归档、链接产物和其他格式不参与修复。
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
    # 即使 -flto 隐藏在响应文件或外部 launcher 中，也不能静默接受 LTO 对象。
    if any(name.startswith('__gnu_lto') for name in names) or 'plugin needed' in symbols.stderr.lower():
        print('faio: MinGW GCC LTO object detected; the TLS workaround requires non-LTO objects.', file=sys.stderr)
        return 1
    if SYMBOL not in names:
        return 0
    # 绝不修改其他强/弱符号，也不合并 __tls_init 的函数体。
    subprocess.run([args.objcopy, '--localize-symbol=' + SYMBOL, str(output)], check=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
