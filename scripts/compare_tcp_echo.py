#!/usr/bin/env python3
"""固定依赖/参数的 TCP echo 对照；同一客户端测 faio 与 Tokio，保留真实 RTT 分位数。

只统计完整且逐字节校验成功的请求，预热不计入；连接建立不计入 RTT。
结果受本机闭环负载发生器与共享 CPU 影响，不能解释为独立机器极限吞吐。
"""
import argparse
import contextlib
import csv
import fcntl
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import re
import signal
import socket
import statistics
import subprocess
import tarfile
import time
from datetime import datetime, timezone


@contextlib.contextmanager
def benchmark_lock(repo):
    """与协程/HTTP suite 共用锁；两个负载同时运行会使对照结果失效。"""
    filename = repo / 'build/benchmark.lock'
    filename.parent.mkdir(parents=True, exist_ok=True)
    with filename.open('w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError('另一个 benchmark 正在运行，请等它结束') from error
        yield


def invoke(argv, cwd, timeout=600):
    return subprocess.run(argv, cwd=cwd, capture_output=True, text=True, check=True, timeout=timeout)


def stop(proc):
    """只结束本次启动的进程组，保留用户现有的其他服务。"""
    if proc is None or proc.poll() is not None:
        return
    os.killpg(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait(timeout=3)


def ready(proc, port):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError('server exited; inspect server log')
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.3) as connection:
                connection.sendall(b'faio benchmark readiness')
                expected = b'faio benchmark readiness'
                actual = b''
                while len(actual) < len(expected):
                    part = connection.recv(len(expected) - len(actual))
                    if not part:
                        raise RuntimeError('early EOF during readiness check')
                    actual += part
                if actual != expected:
                    raise RuntimeError('echo verification failed')
                return
        except (ConnectionRefusedError, TimeoutError):
            time.sleep(.1)
    raise RuntimeError('server startup timed out')


def measure_suite(args, repo):
    """在持有 suite 锁时构建、顺序运行、保存全部原始指标并汇总。"""
    connections = [int(v) for v in args.connections.split(',')]
    payloads = [int(v) for v in args.payloads.split(',')]
    build = (args.build or repo / 'build' / (
        'macos-clang23' if platform.system() == 'Darwin' else 'linux-dual')).resolve()
    out = (args.output or repo / 'benchmark/result/tcp_echo' / datetime.now(timezone.utc).strftime(
        '%Y%m%dT%H%M%SZ')).resolve()
    out.mkdir(parents=True, exist_ok=False)
    compiled = invoke(['cmake', '--build', str(build), '--target', 'faio_tcp_benchmark', 'tcp_echo_load', '-j4'], repo)
    (out / 'cpp_build.log').write_text(compiled.stdout + compiled.stderr, encoding='utf-8')
    rust = repo / 'benchmark/tcp/tokio-benchmark'
    rust_target = build / 'tokio-tcp'
    compiled = invoke(['cargo', 'build', '--release', '--locked', '--target-dir', str(rust_target)], rust)
    (out / 'rust_build.log').write_text(compiled.stdout + compiled.stderr, encoding='utf-8')
    binaries = {'faio': build / 'benchmark/faio_tcp_benchmark', 'tokio': rust_target / 'release/tokio-tcp-benchmark'}
    client = build / 'benchmark/tcp_echo_load'
    source_files = sorted((repo / 'include').rglob('*.hpp'))
    source_files += [repo / 'CMakeLists.txt', repo / 'benchmark/CMakeLists.txt', Path(__file__).resolve(),
                     repo / 'benchmark/tcp/faio_tcp_benchmark.cpp', repo / 'benchmark/tcp/tcp_echo_load.cpp',
                     rust / 'Cargo.toml', rust / 'Cargo.lock', rust / 'src/main.rs']
    with tarfile.open(out / 'source_snapshot.tar.gz', 'w:gz') as snapshot:
        for source in source_files:
            snapshot.add(source, arcname=str(source.relative_to(repo)))
    metadata = {'created_utc': datetime.now(timezone.utc).isoformat(), 'platform': platform.platform(),
                'machine': platform.machine(),
                'cpu_count': os.cpu_count(),
                'affinity': sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None,
                'arguments': {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                'policy': 'O2/no LTO; same echo framing, TCP_NODELAY=true, 64KiB receive buffers, same native client; alternate server order; one outstanding request/connection; excludes connect/warmup',
                'faio_write_mode': 'inline partial-write diagnostic' if args.faio_inline_write else 'write_all public API',
                'faio_accept_placement': 'balanced first registration; registered resource ownership remains stable',
                'requested_faio_backend': args.io_backend,
                'acceptance': f'faio/Tokio throughput >= {args.min_throughput_ratio} AND p50/p99 latency <= {args.max_latency_ratio}; every configuration must pass',
                'tokio_version': '1.49.0',
                'cargo_lock_sha256': hashlib.sha256((rust / 'Cargo.lock').read_bytes()).hexdigest(),
                'binary_sha256': {k: hashlib.sha256(v.read_bytes()).hexdigest() for k, v in binaries.items()},
                'source_sha256': {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                                  source_files},
                'client_binary_sha256': hashlib.sha256(client.read_bytes()).hexdigest()}
    for key, argv in [('rustc', ['rustc', '--version']), ('kernel', ['uname', '-a'])]:
        metadata[key] = invoke(argv, repo).stdout.strip()
    compiler = next(line.split('=', 1)[1] for line in (build / 'CMakeCache.txt').read_text().splitlines()
                    if line.startswith('CMAKE_CXX_COMPILER:'))
    metadata['compiler'] = invoke([compiler, '--version'], repo).stdout.strip()
    metadata['cpu'] = invoke(['sysctl', '-n', 'machdep.cpu.brand_string'] if platform.system() == 'Darwin'
                             else ['lscpu'], repo).stdout.strip()
    (out / 'metadata.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding='utf-8')
    compile_file = build / 'compile_commands.json'
    if compile_file.exists():
        (out / 'compile_commands.json').write_bytes(compile_file.read_bytes())
    # 缓存和 Ninja 真实命令都保存；已有 compile_commands 可能来自旧配置。
    (out / 'CMakeCache.txt').write_bytes((build / 'CMakeCache.txt').read_bytes())
    if (build / 'build.ninja').exists():
        exact_commands = invoke(['ninja', '-C', str(build), '-t', 'commands', 'faio_tcp_benchmark', 'tcp_echo_load'],
                                repo).stdout
        (out / 'cpp_compile_link_commands.txt').write_text(exact_commands, encoding='utf-8')
        if '-O2' not in exact_commands or any(flag in exact_commands for flag in ['-O3', '-flto', '-fsanitize', '-pg']):
            raise RuntimeError(
                'TCP acceptance requires matching O2 builds without LTO/sanitizer/profiler instrumentation')
    rows, commands = [], []
    for number in range(1, args.rounds + 1):
        for count in connections:
            for payload in payloads:
                for name in (['faio', 'tokio'] if number % 2 else ['tokio', 'faio']):
                    # 动态选空闲端口，检查失败时不结束占用该端口的其他服务。
                    with socket.socket() as selector:
                        selector.bind(('127.0.0.1', 0))
                        port = selector.getsockname()[1]
                    server_argv = [str(binaries[name]), '127.0.0.1', str(port), str(args.workers),
                                   'echo_inline' if name == 'faio' and args.faio_inline_write else 'echo']
                    if name == 'faio' and args.io_backend != 'default':
                        server_argv.append('--io-backend=' + args.io_backend)
                    client_argv = [str(client), '127.0.0.1', str(port), str(min(count, args.client_threads)),
                                   str(count), str(payload), str(args.seconds), str(args.warmup)]
                    label = f'{name}_r{number}_c{count}_b{payload}'
                    print(f'[measure] {label}', flush=True)
                    proc = None
                    commands.append(
                        {'round': number, 'implementation': name, 'server': server_argv, 'client': client_argv})
                    # 每次测量前写入命令；中途失败也可复现已完成与失败的运行。
                    (out / 'commands.json').write_text(json.dumps(commands, indent=2), encoding='utf-8')
                    try:
                        with (out / f'{label}.server.log').open('w') as log:
                            server_env = os.environ.copy()
                            # 参数扫描只改变 faio 的显式配置；命令、参数和环境均随结果保存。
                            for option, variable in [('faio_io_interval', 'FAIO_BENCH_IO_INTERVAL'),
                                                     ('faio_idle_spin_count', 'FAIO_BENCH_IDLE_SPIN_COUNT'),
                                                     ('faio_max_io_delay_us', 'FAIO_BENCH_MAX_IO_DELAY_US')]:
                                value = getattr(args, option)
                                if value is not None and name == 'faio':
                                    server_env[variable] = str(value)
                                else:
                                    server_env.pop(variable, None)
                            proc = subprocess.Popen(server_argv, cwd=repo, env=server_env, start_new_session=True,
                                                    stdout=log, stderr=subprocess.STDOUT)
                            ready(proc, port)
                            if name == 'faio':
                                selected = re.search(r'faio tcp benchmark IO backend: ([a-z]+)',
                                                     (out / f'{label}.server.log').read_text())
                                if selected is None:
                                    raise RuntimeError('faio server did not report its actual IO backend')
                                actual_backend = selected.group(1)
                                if args.io_backend != 'default' and actual_backend != args.io_backend:
                                    raise RuntimeError(
                                        'faio selected a backend different from the explicit comparison request')
                            else:
                                actual_backend = 'mio'
                            try:
                                completed = invoke(client_argv, repo, timeout=args.seconds + args.warmup + 30)
                            except subprocess.CalledProcessError as failure:
                                (out / f'{label}.failed.stdout.log').write_text(failure.stdout or '', encoding='utf-8')
                                (out / f'{label}.stderr.log').write_text(failure.stderr or '', encoding='utf-8')
                                raise
                            (out / f'{label}.csv').write_text(completed.stdout, encoding='utf-8')
                            (out / f'{label}.stderr.log').write_text(completed.stderr, encoding='utf-8')
                            result = list(csv.DictReader(io.StringIO(completed.stdout)))
                            if len(result) != 1 or int(result[0]['errors']):
                                raise RuntimeError('incomplete or erroneous client measurement')
                            measurements = result[0]
                            if int(measurements['connections']) != count or int(
                                    measurements['payload_bytes']) != payload:
                                raise RuntimeError('client configuration differs from requested workload')
                            numeric = [float(v) for v in measurements.values()]
                            if not all(math.isfinite(v) and v >= 0 for v in numeric) or int(
                                    measurements['requests']) <= 0:
                                raise RuntimeError('invalid client result')
                            if not (0 < float(measurements['p50_ns']) <= float(measurements['p95_ns']) <=
                                    float(measurements['p99_ns']) <= float(measurements['p999_ns']) <= float(
                                        measurements['max_ns'])):
                                raise RuntimeError('latency percentiles are inconsistent')
                            rows.append(dict(implementation=name, io_backend=actual_backend, round=number,
                                             server_workers=args.workers, **result[0]))
                            if proc.poll() is not None:
                                raise RuntimeError('server exited during measurement')
                    finally:
                        stop(proc)
    with (out / 'all_runs.csv').open('w', newline='') as destination:
        writer = csv.DictWriter(destination, fieldnames=list(rows[0]));
        writer.writeheader();
        writer.writerows(rows)
    metrics = ['requests_per_sec', 'p50_ns', 'p95_ns', 'p99_ns', 'p999_ns', 'max_ns']
    summary, comparison = [], []
    for count in connections:
        for payload in payloads:
            means = {}
            for name in binaries:
                group = [r for r in rows if r['implementation'] == name and int(r['connections']) == count and int(
                    r['payload_bytes']) == payload]
                if len(group) != args.rounds:
                    raise RuntimeError('missing measurement rounds')
                backends = {r['io_backend'] for r in group}
                if len(backends) != 1:
                    raise RuntimeError('backend changed between rounds')
                row = dict(implementation=name, io_backend=backends.pop(), connections=count, payload_bytes=payload,
                           server_workers=args.workers, rounds=args.rounds)
                for metric in metrics:
                    values = [float(r[metric]) for r in group]
                    row[metric] = statistics.mean(values)
                    row[f'{metric}_stddev'] = statistics.stdev(values) if len(values) > 1 else 0
                summary.append(row);
                means[name] = row
            ratio = means['faio']['requests_per_sec'] / means['tokio']['requests_per_sec']
            p50_ratio = means['faio']['p50_ns'] / means['tokio']['p50_ns']
            p99_ratio = means['faio']['p99_ns'] / means['tokio']['p99_ns']
            comparison.append(dict(connections=count, payload_bytes=payload, faio_over_tokio_throughput=ratio,
                                   throughput_gap_percent=max(0, 100 * (1 - ratio)),
                                   passes_throughput=ratio >= args.min_throughput_ratio,
                                   passes_p50=p50_ratio <= args.max_latency_ratio,
                                   passes_p99=p99_ratio <= args.max_latency_ratio,
                                   passes_20_percent=ratio >= .8 and p50_ratio <= 1.2 and p99_ratio <= 1.2,
                                   passes_target=ratio >= args.min_throughput_ratio and p50_ratio <= args.max_latency_ratio and p99_ratio <= args.max_latency_ratio,
                                   faio_over_tokio_p50=p50_ratio,
                                   faio_over_tokio_p95=means['faio']['p95_ns'] / means['tokio']['p95_ns'],
                                   faio_over_tokio_p99=p99_ratio))
    for filename, data in [('summary.csv', summary), ('ratios.csv', comparison)]:
        with (out / filename).open('w', newline='') as destination:
            writer = csv.DictWriter(destination, fieldnames=list(data[0]));
            writer.writeheader();
            writer.writerows(data)
    text = ['# TCP echo 性能对照', '',
            '相同 worker 数、缓冲区、Nagle 设置和原生客户端；吞吐为完整有效回包/秒。各分位数取每轮实测分位数的算术均值。',
            '',
            f'验收同时要求 faio 吞吐至少为 Tokio 的 {100 * args.min_throughput_ratio:g}%，P50 与 P99 不高于 Tokio 的 {100 * args.max_latency_ratio:g}%。',
            '',
            '|连接数|字节数|faio/Tokio 吞吐|吞吐差距|达到目标|p50 比值|p95 比值|p99 比值|',
            '|---:|---:|---:|---:|:---:|---:|---:|---:|']
    for row in comparison:
        text.append(
            f"|{row['connections']}|{row['payload_bytes']}|{row['faio_over_tokio_throughput']:.3f}|{row['throughput_gap_percent']:.1f}%|{row['passes_target']}|{row['faio_over_tokio_p50']:.3f}|{row['faio_over_tokio_p95']:.3f}|{row['faio_over_tokio_p99']:.3f}|")
    if args.faio_inline_write:
        text += ['', '本轮显式启用 faio 内联完整写循环诊断模式，默认公开 write_all 的正式对照保存在其他目录。']
    text += ['',
             '这是同机闭环测量，共享 CPU 和客户端容量会限制吞吐；结果适用于记录的环境与负载。原始数据、参数、源码快照、二进制哈希和环境保留在本目录。',
             '']
    (out / 'report.md').write_text('\n'.join(text), encoding='utf-8')
    (out / 'acceptance.json').write_text(json.dumps({'min_throughput_ratio': args.min_throughput_ratio,
                                                     'max_latency_ratio': args.max_latency_ratio,
                                                     'all_passed': all(r['passes_target'] for r in comparison),
                                                     'failed_configurations': [r for r in comparison if
                                                                               not r['passes_target']]}, indent=2),
                                         encoding='utf-8')
    print(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, help='Use configured CMake build directory')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--warmup', type=float, default=1)
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--client-threads', type=int, default=4)
    parser.add_argument('--connections', default='1,64,256')
    parser.add_argument('--payloads', default='64,1024,16384')
    parser.add_argument('--min-throughput-ratio', type=float, default=.95)
    parser.add_argument('--max-latency-ratio', type=float, default=1.05)
    parser.add_argument('--faio-io-interval', type=int, help='Diagnostic: explicit faio IO fairness interval')
    parser.add_argument('--faio-idle-spin-count', type=int, help='Diagnostic: explicit faio bounded idle polling')
    parser.add_argument('--faio-max-io-delay-us', type=int,
                        help='Diagnostic: explicit faio IO drive time budget in microseconds')
    parser.add_argument('--io-backend', choices=['default', 'epoll', 'uring'], default='default',
                        help='Explicit faio Linux backend; server logs must confirm the requested choice')
    parser.add_argument('--faio-inline-write', action='store_true',
                        help='Diagnostic only: inline the complete-write loop in the faio connection task')
    args = parser.parse_args()
    if (not 0 < args.min_throughput_ratio <= 1 or not 1 <= args.max_latency_ratio or
            not math.isfinite(args.max_latency_ratio) or
            (args.faio_io_interval is not None and args.faio_io_interval < 1) or
            (args.faio_idle_spin_count is not None and args.faio_idle_spin_count < 0) or
            (args.faio_max_io_delay_us is not None and not 1 <= args.faio_max_io_delay_us <= 1_000_000)):
        parser.error('invalid acceptance thresholds or diagnostic scheduler configuration')
    if args.io_backend != 'default' and platform.system() != 'Linux':
        parser.error('--io-backend=epoll|uring requires Linux')
    connections = [int(v) for v in args.connections.split(',')]
    payloads = [int(v) for v in args.payloads.split(',')]
    if (min(args.rounds, args.workers, args.client_threads, *connections, *payloads) < 1 or
            not math.isfinite(args.seconds) or not math.isfinite(args.warmup) or args.seconds < 3 or args.warmup < 1):
        parser.error('positive configuration, seconds >= 3, and warmup >= 1 required')
    repo = Path(__file__).resolve().parents[1]
    with benchmark_lock(repo):
        measure_suite(args, repo)


if __name__ == '__main__':
    main()
