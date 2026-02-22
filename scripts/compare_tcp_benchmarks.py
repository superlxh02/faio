#!/usr/bin/env python3
import argparse
import os
import re
import signal
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import matplotlib.pyplot as plt
import pandas as pd


@dataclass
class BenchResult:
    runtime: str
    requests_per_sec: float
    latency_avg_ms: float
    latency_p50_ms: float
    latency_p90_ms: float
    latency_p99_ms: float
    timeout_count: int
    transfer_per_sec: str


def run_cmd(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True, check=True)


def free_port(port: int) -> None:
    pids: set[int] = set()
    for tool in (["lsof", "-ti", f"tcp:{port}"], ["fuser", "-n", "tcp", str(port)]):
        res = subprocess.run(tool, capture_output=True, text=True, check=False)
        text = (res.stdout or "") + " " + (res.stderr or "")
        for token in text.split():
            if token.isdigit():
                pids.add(int(token))
    for pid in sorted(pids):
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    time.sleep(0.2)
    for pid in sorted(pids):
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def stop_process(proc: Optional[subprocess.Popen]) -> None:
    if not proc or proc.poll() is not None:
        return
    try:
        pgid = os.getpgid(proc.pid)
        os.killpg(pgid, signal.SIGTERM)
        proc.wait(timeout=4)
    except Exception:
        try:
            os.killpg(pgid, signal.SIGKILL)
            proc.wait(timeout=4)
        except Exception:
            pass


def wait_ready(url: str, timeout_sec: float = 15.0) -> None:
    import urllib.request
    import urllib.error

    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=1):
                return
        except (urllib.error.URLError, TimeoutError):
            time.sleep(0.1)
    raise RuntimeError(f"server not ready: {url}")


def parse_wrk(output: str, name: str) -> BenchResult:
    def to_ms(value: float, unit: str) -> float:
        if unit == "us":
            return value / 1000.0
        if unit == "s":
            return value * 1000.0
        return value

    def parse_percentile(percentile: int) -> float:
        m = re.search(rf"^\s*{percentile}(?:\.0+)?%\s+([0-9.]+)\s*(us|ms|s)\s*$", output, re.MULTILINE)
        if not m:
            raise RuntimeError(f"failed parse wrk percentile {percentile}% for {name}\\n{output}")
        return to_ms(float(m.group(1)), m.group(2))

    req = re.search(r"Requests/sec:\s+([0-9.]+)", output)
    lat = re.search(r"Latency\s+([0-9.]+)(ms|us|s)", output)
    trf = re.search(r"Transfer/sec:\s+([^\n]+)", output)
    timeout = re.search(r"Socket errors:.*timeout\s+(\d+)", output)
    if not req or not lat or not trf:
        raise RuntimeError(f"failed parse wrk output for {name}\n{output}")

    lat_ms = to_ms(float(lat.group(1)), lat.group(2))

    return BenchResult(
        runtime=name,
        requests_per_sec=float(req.group(1)),
        latency_avg_ms=lat_ms,
        latency_p50_ms=parse_percentile(50),
        latency_p90_ms=parse_percentile(90),
        latency_p99_ms=parse_percentile(99),
        timeout_count=int(timeout.group(1)) if timeout else 0,
        transfer_per_sec=trf.group(1).strip(),
    )


def run_wrk(wrk_bin: str, threads: int, conns: int, duration: str, url: str, cwd: Path) -> str:
    cmd = [wrk_bin, "--latency", f"-t{threads}", f"-c{conns}", f"-d{duration}", url]
    out = run_cmd(cmd, cwd)
    return out.stdout


def save_report(results: list[BenchResult], result_dir: Path) -> None:
    result_dir.mkdir(parents=True, exist_ok=True)
    df = pd.DataFrame([r.__dict__ for r in results])
    df.to_csv(result_dir / "summary.csv", index=False)

    md = [
        "# TCP(HTTP-like) Benchmark Compare",
        "",
        "| Runtime | Requests/sec | Avg(ms) | P50(ms) | P90(ms) | P99(ms) | Timeout | Transfer/sec |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for r in results:
        md.append(
            f"| {r.runtime} | {r.requests_per_sec:.2f} | {r.latency_avg_ms:.2f} | {r.latency_p50_ms:.2f} | {r.latency_p90_ms:.2f} | {r.latency_p99_ms:.2f} | {r.timeout_count} | {r.transfer_per_sec} |"
        )
    (result_dir / "summary.md").write_text("\n".join(md) + "\n", encoding="utf-8")

    fig, axes = plt.subplots(1, 4, figsize=(18, 4))
    names = [r.runtime for r in results]
    axes[0].bar(names, [r.requests_per_sec for r in results])
    axes[0].set_title("Requests/sec")
    axes[1].bar(names, [r.latency_avg_ms for r in results])
    axes[1].set_title("Latency Avg (ms)")
    axes[2].bar(names, [r.latency_p99_ms for r in results])
    axes[2].set_title("Latency P99 (ms)")
    axes[3].bar(names, [r.timeout_count for r in results])
    axes[3].set_title("Timeout")
    fig.tight_layout()
    fig.savefig(result_dir / "comparison.png", dpi=140)
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser(description="TCP benchmark compare: asio vs faio-tcp vs tokio-tcp")
    parser.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent))
    parser.add_argument("--wrk", default="wrk")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--connections", type=int, default=5000)
    parser.add_argument("--duration", default="60s")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    build_dir = repo / "build"
    tokio_dir = repo / "benchmark" / "tcp" / "tokio-benchmark"
    result_dir = repo / "benchmark"  / "result" / "tcp"

    targets = [
        {
            "name": "C++(asio)",
            "build": lambda: run_cmd(["cmake", "--build", str(build_dir), "-j4", "--target", "asio_tcp_benchmark"], repo),
            "start": lambda: subprocess.Popen([str(build_dir / "benchmark" / "asio_tcp_benchmark"), "0.0.0.0", "10090"], cwd=str(repo), preexec_fn=os.setsid),
            "health": "http://127.0.0.1:10090/health",
            "url": "http://127.0.0.1:10090/index",
            "port": 10090,
        },
        {
            "name": "C++(faio-tcp)",
            "build": lambda: run_cmd(["cmake", "--build", str(build_dir), "-j4", "--target", "faio_tcp_benmark"], repo),
            "start": lambda: subprocess.Popen([str(build_dir / "benchmark" / "faio_tcp_benmark")], cwd=str(repo), preexec_fn=os.setsid),
            "health": "http://127.0.0.1:18081/health",
            "url": "http://127.0.0.1:18081/index",
            "port": 18081,
        },
        {
            "name": "Rust(tokio)",
            "build": lambda: run_cmd(["cargo", "build", "--release"], tokio_dir),
            "start": lambda: subprocess.Popen(["cargo", "run", "--release", "--", "0.0.0.0", "10092"], cwd=str(tokio_dir), preexec_fn=os.setsid),
            "health": "http://127.0.0.1:10092/health",
            "url": "http://127.0.0.1:10092/index",
            "port": 10092,
        },
    ]

    results: list[BenchResult] = []
    running: Optional[subprocess.Popen] = None

    try:
        for target in targets:
            print(f"[build] {target['name']}")
            free_port(target["port"])
            target["build"]()

            print(f"[run] {target['name']}")
            running = target["start"]()
            wait_ready(target["health"])
            wrk_out = run_wrk(args.wrk, args.threads, args.connections, args.duration, target["url"], repo)
            results.append(parse_wrk(wrk_out, target["name"]))
            stop_process(running)
            running = None

        save_report(results, result_dir)
        print(f"report saved in: {result_dir}")
        return 0
    finally:
        stop_process(running)


if __name__ == "__main__":
    raise SystemExit(main())
