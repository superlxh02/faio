#!/usr/bin/env python3
"""启动本次构建的常驻 echo 示例并验证回包；结束时只清理自己启动的进程。"""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--protocol', required=True, choices=['tcp', 'udp'])
    parser.add_argument('--port', required=True, type=int)
    args = parser.parse_args()
    # 原示例采用固定端口；测试前先确保它未被其他用户进程占用。
    kind = socket.SOCK_STREAM if args.protocol == 'tcp' else socket.SOCK_DGRAM
    with socket.socket(socket.AF_INET, kind) as probe:
        probe.bind(('127.0.0.1', args.port))
    proc = subprocess.Popen([str(args.executable.resolve())], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    try:
        deadline = time.monotonic() + 10
        payloads = [b'faio example verification\n', bytes(range(256)) * 4]
        while True:
            if proc.poll() is not None:
                raise RuntimeError('example exited: ' + proc.stdout.read().decode(errors='replace'))
            try:
                with socket.socket(socket.AF_INET, kind) as client:
                    client.settimeout(1)
                    client.connect(('127.0.0.1', args.port))
                    for payload in payloads:
                        client.sendall(payload)
                        if args.protocol == 'tcp':
                            received = b''
                            while len(received) < len(payload):
                                part = client.recv(len(payload) - len(received))
                                if not part:
                                    raise RuntimeError('early EOF')
                                received += part
                        else:
                            received = client.recv(65536)
                        if received != payload:
                            raise RuntimeError('echo bytes differ')
                break
            except (ConnectionRefusedError, TimeoutError, ConnectionResetError):
                if time.monotonic() >= deadline:
                    raise RuntimeError('example readiness/echo timed out')
                time.sleep(.1)
        if proc.poll() is not None:
            raise RuntimeError('example exited during verification')
        print(f'PASS {args.executable.name}: {args.protocol} exact echo, two messages')
    finally:
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)
        proc.stdout.close()


if __name__ == '__main__':
    main()
