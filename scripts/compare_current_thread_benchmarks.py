#!/usr/bin/env python3
"""Run current-thread and one-worker benchmarks; retain every CSV and metadata."""
import argparse
import csv
import datetime
import hashlib
import json
from pathlib import Path
import platform
import resource
import statistics
import subprocess
import time


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--count', type=int, default=20000)
    parser.add_argument('--rounds', type=int, default=5)
    parser.add_argument('--baseline', type=Path, help='Previously compiled faio baseline executable')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.count < 32 or args.rounds < 1:
        parser.error('count >= 32 and rounds >= 1 are required')
    root = Path(__file__).resolve().parents[1]
    if platform.system() != 'Linux':
        parser.error('Run on Linux, or run this script inside faio:dev with seccomp=unconfined')
    output = args.output or root / 'benchmark/result/current_thread' / datetime.datetime.now(
        datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    output.mkdir(parents=True, exist_ok=False)
    build = root / 'build/current-thread-optimized'
    run(['cmake', '-S', str(root), '-B', str(build), '-G', 'Ninja',
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG',
         '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF', '-DFAIO_INSTALL=OFF'])
    run(['cmake', '--build', str(build), '--target', 'faio_current_thread_benchmark', '-j', '4'])
    rust_build = root / 'build/current-thread-rust'
    run(['cargo', 'build', '--release', '--locked', '--offline', '--target-dir', str(rust_build),
         '--manifest-path', str(root / 'benchmark/coro/tokio-benchmark/Cargo.toml'),
         '--bin', 'current_thread'])
    binaries = {'faio': build / 'benchmark/faio_current_thread_benchmark',
                'tokio': rust_build / 'release/current_thread'}
    if args.baseline:
        binaries['before'] = args.baseline.resolve()
    cache = (build / 'CMakeCache.txt').read_text().splitlines()
    compiler = next(line.split('=', 1)[1] for line in cache
                    if line.startswith(('CMAKE_CXX_COMPILER:FILEPATH=', 'CMAKE_CXX_COMPILER:STRING=')))
    compile_commands = json.loads((build / 'compile_commands.json').read_text())
    metadata = {
        'utc_started': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'platform': platform.platform(), 'machine': platform.machine(),
        'count': args.count, 'rounds': args.rounds, 'blocking_threads': 4,
        'compiler': run([compiler, '--version'], capture_output=True).stdout.splitlines()[0],
        'benchmark_compile_command': next(entry for entry in compile_commands
                                          if entry['file'].endswith('/current_thread_benchmark.cpp')),
        'rustc': run(['rustc', '--version'], capture_output=True).stdout.strip(),
        'git_head': run(['git', '-C', str(root), 'rev-parse', 'HEAD'], capture_output=True).stdout.strip(),
        'git_status': run(['git', '-C', str(root), 'status', '--short'], capture_output=True).stdout,
        'binaries_sha256': {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in binaries.items()},
        'headers_sha256': {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in sorted((root / 'include/faio').rglob('*.hpp'))},
        'policy': 'Sequential runs, alternating executable order each round; O2, no LTO; unpinned Linux VM',
    }
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    rows = []
    usage = []
    for repeat in range(args.rounds):
        order = list(binaries.items())
        if repeat % 2:
            order.reverse()
        for mode in ('current', 'multi'):
            for timing in ('batch', 'sample'):
                for name, binary in order:
                    destination = output / f'{name}_{mode}_{timing}_{repeat + 1}.csv'
                    cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN)
                    wall_started = time.monotonic()
                    with destination.open('w') as out:
                        run([str(binary), str(args.count), timing, mode], stdout=out, timeout=60)
                    cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
                    usage.append(dict(implementation=name, mode=mode, timing=timing, repeat=repeat + 1,
                                      wall_seconds=time.monotonic() - wall_started,
                                      user_seconds=cpu_after.ru_utime - cpu_before.ru_utime,
                                      system_seconds=cpu_after.ru_stime - cpu_before.ru_stime))
                    with destination.open() as source:
                        for row in csv.DictReader(source):
                            rows.append(dict(implementation=name, mode=mode, timing=timing,
                                             repeat=repeat + 1, **row))
        print(f'Completed round {repeat + 1}/{args.rounds}', flush=True)
    with (output / 'all_runs.csv').open('w', newline='') as out:
        writer = csv.DictWriter(out, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    with (output / 'process_usage.csv').open('w', newline='') as out:
        writer = csv.DictWriter(out, fieldnames=list(usage[0]))
        writer.writeheader()
        writer.writerows(usage)
    groups = {}
    for row in rows:
        key = tuple(row[k] for k in ('implementation', 'mode', 'timing', 'scenario'))
        groups.setdefault(key, []).append(row)
    summary = []
    for key, group in groups.items():
        summary.append(dict(zip(('implementation', 'mode', 'timing', 'scenario'), key),
                            **{field: statistics.median(float(r[field]) for r in group)
                               for field in ('ns_per_op', 'p50_ns', 'p99_ns', 'p999_ns', 'max_ns')}))
    with (output / 'summary.csv').open('w', newline='') as out:
        writer = csv.DictWriter(out, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    print(output)


if __name__ == '__main__':
    main()
