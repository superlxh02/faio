#!/usr/bin/env python3
"""协程与 TCP benchmark 共用的数据校验、算术均值统计与 Python 图表。

所有数值均保留在 CSV 中；PNG 按场景类别拆分，避免把不同量纲混在同一坐标轴。
标准差采用样本标准差（ddof=1）；均值是每轮指标的算术均值，不是混合样本分位数。
"""
import contextlib
import fcntl
import json
import os
import platform
import subprocess
import tarfile
import hashlib
from datetime import datetime, timezone
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


@contextlib.contextmanager
def benchmark_lock(repo):
    """同一 checkout 的协程与 TCP 测试共用进程锁，不允许相互争抢 CPU。"""
    lock_path = repo / "build" / "benchmark.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise RuntimeError("另一个 benchmark 正在运行；请等它结束") from exc
        yield


def command(cmd, cwd, timeout=600):
    return subprocess.run(cmd, cwd=cwd, text=True, capture_output=True, check=True, timeout=timeout)


def configure_cpp(repo):
    build = repo / "build" / "benchmark-o2"
    command(["cmake", "-S", str(repo), "-B", str(build), "-DCMAKE_CXX_COMPILER=g++",
             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG",
             "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF"], repo)
    # 记录真正被 CMake 识别的编译器，不能只相信程序名 g++。
    compiler_file = next((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if 'set(CMAKE_CXX_COMPILER_ID "GNU")' not in compiler_file.read_text():
        raise RuntimeError("benchmark 要求 GCC；当前 g++ 不是 GNU C++ 编译器")
    return build


def create_output(repo, suite, override=None):
    base = repo / "benchmark" / "result" / suite
    out = Path(override).resolve() if override else base / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out.mkdir(parents=True, exist_ok=False)
    return out


def metadata(repo, out, args, extra):
    info = {"created_utc": datetime.now(timezone.utc).isoformat(), "platform": platform.platform(),
            "cpu_count": os.cpu_count(), "affinity": sorted(os.sched_getaffinity(0)),
            "arguments": vars(args), "cpp_flags": "GCC -O2 -DNDEBUG, no LTO",
            "rust_flags": "rustc opt-level=2, lto=false, codegen-units=1",
            "statistics": "arithmetic mean; sample stddev ddof=1; CV=stddev/mean", **extra}
    for name, cmd in [("gcc", ["g++", "--version"]), ("rustc", ["rustc", "--version"]),
                      ("cargo", ["cargo", "--version"]),
                      ("kernel", ["uname", "-a"]), ("cpu", ["lscpu"]),
                      ("git_head", ["git", "rev-parse", "HEAD"]), ("git_status", ["git", "status", "--short"])]:
        try:
            info[name] = command(cmd, repo).stdout
        except (FileNotFoundError, subprocess.CalledProcessError):
            info[name] = "unavailable"
    # 未提交源码也必须可复现：保存本次运行的库头文件、测试源码、脚本与依赖锁。
    files=list((repo/'include').rglob('*.hpp'))+list((repo/'scripts').glob('*.py'))
    files += [repo/'CMakeLists.txt',repo/'benchmark/CMakeLists.txt']
    for folder in ['coro','tcp']:
        for path in (repo/'benchmark'/folder).rglob('*'):
            if path.is_file() and 'target' not in path.parts and path.suffix in {'.cpp','.rs','.toml','.lock','.md'}:files.append(path)
    info['source_sha256']={str(f.relative_to(repo)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
    with tarfile.open(out/'source_snapshot.tar.gz','w:gz') as bundle:
        for file in files:bundle.add(file,arcname=str(file.relative_to(repo)))
    (out / "metadata.json").write_text(json.dumps(info, ensure_ascii=False, indent=2), encoding="utf-8")
    # 将编译命令也归档，方便确认实际没有 -O3/LTO 混入。
    compiles = repo / "build/benchmark-o2/compile_commands.json"
    if compiles.exists():
        (out / "compile_commands.json").write_bytes(compiles.read_bytes())


def _axes(metrics):
    columns = min(3, len(metrics)); rows = (len(metrics)+columns-1)//columns
    fig, axes = plt.subplots(rows, columns, figsize=(7*columns, 4.4*rows), squeeze=False)
    for ax in axes.flat[len(metrics):]: ax.set_visible(False)
    return fig, list(axes.flat)


def _scale(ax, values):
    values = np.asarray(values)
    if np.all(values > 0) and values.max()/values.min() > 12:
        ax.set_yscale("log")
        ax.set_ylabel("log scale")
    ax.grid(axis="y", alpha=.2)


def round_report(df, out, metrics):
    """每轮 CSV + 全部指标柱状图、折线图；类别内指标各用自己的坐标轴。"""
    out.mkdir(parents=True, exist_ok=True)
    df.to_csv(out / "comparison.csv", index=False, float_format="%.6f")
    for category, group in df.groupby("category", sort=False):
        scenarios = list(group.scenario.unique()); implementations = list(group.implementation.unique())
        for kind in ["bar", "line"]:
            fig, axes = _axes(metrics)
            for ax, metric in zip(axes, metrics):
                matrix = group.pivot(index="scenario", columns="implementation", values=metric).reindex(scenarios)
                x = np.arange(len(scenarios)); width = .8/len(implementations)
                for j, impl in enumerate(implementations):
                    y = matrix[impl].to_numpy()
                    if kind == "bar": ax.bar(x+(j-(len(implementations)-1)/2)*width, y, width, label=impl)
                    else: ax.plot(x, y, "o-", label=impl, markersize=3)
                _scale(ax, matrix.to_numpy().ravel())
                ax.set_title(metric); ax.set_xticks(x, scenarios, rotation=65, ha="right", fontsize=7)
            axes[0].legend(fontsize=8)
            fig.suptitle(f"{out.name}: {category} / {kind}")
            fig.tight_layout(rect=(0, 0, 1, .98))
            fig.savefig(out / f"{category}_{kind}.png", dpi=120); plt.close(fig)
    # 横轴是场景编号的折线仅便于比较，不代表时间演进；真正的跨轮波动在汇总图。


def latency_curves(samples_root, out):
    """逐次原始样本全部归档；画真实延迟分布（经验分位数曲线），保留尾部。"""
    implementations = [p.name for p in samples_root.iterdir() if p.is_dir()]
    scenarios = sorted(p.stem for p in (samples_root / implementations[0]).glob("*.csv.gz"))
    scenarios = [s.removesuffix('.csv') for s in scenarios]
    for page in range(0, len(scenarios), 12):
        names = scenarios[page:page+12]
        fig, axes = plt.subplots(4, 3, figsize=(17, 13), squeeze=False)
        for ax, name in zip(axes.flat, names):
            for impl in implementations:
                data = pd.read_csv(samples_root / impl / f"{name}.csv.gz").latency_ns.to_numpy()
                q = np.r_[np.linspace(0, .99, 100), .995, .999, 1.]
                ax.plot(q*100, np.quantile(data, q, method="lower"), label=impl)
            ax.set_title(name, fontsize=8);ax.set_xlabel("percentile (%)");ax.set_ylabel("latency (ns)")
            ax.set_yscale("symlog", linthresh=1);ax.grid(alpha=.2)
        for ax in list(axes.flat)[len(names):]: ax.set_visible(False)
        axes.flat[0].legend();fig.tight_layout()
        fig.savefig(out / f"latency_distribution_{page//12+1:02}.png", dpi=120);plt.close(fig)


def summary_report(frames, out, metrics, expected_rounds):
    all_rows = pd.concat(frames, ignore_index=True)
    keys = ["category", "scenario", "implementation"]
    counts = all_rows.groupby(keys).size()
    if not (counts == expected_rounds).all(): raise RuntimeError("轮次数据缺失，拒绝产生部分均值")
    if not np.isfinite(all_rows[metrics].to_numpy()).all(): raise RuntimeError("结果含 NaN/Inf")
    all_rows.to_csv(out / "all_rounds.csv", index=False, float_format="%.6f")
    rows = []
    for key, group in all_rows.groupby(keys, sort=False):
        for metric in metrics:
            values = group[metric]; mean = values.mean(); std = values.std(ddof=1)
            rows.append(dict(zip(keys, key)) | {"metric": metric, "rounds": len(values), "mean": mean,
                        "stddev": std, "cv_percent": 100*std/mean if mean else 0.,
                        "min": values.min(), "max": values.max(), "range": values.max()-values.min(),
                        "median": values.median()})
    statistics = pd.DataFrame(rows)
    statistics.to_csv(out / "statistics.csv", index=False, float_format="%.6f")
    means = all_rows.groupby(keys, sort=False)[metrics].mean().reset_index()
    means.to_csv(out / "summary.csv", index=False, float_format="%.6f")
    round_report(means, out / "mean", metrics)
    for category, group in statistics.groupby("category", sort=False):
        fig, axes = _axes(metrics);scenarios=list(group.scenario.unique());impls=list(group.implementation.unique())
        for ax, metric in zip(axes, metrics):
            values=group[group.metric==metric];width=.8/len(impls);x=np.arange(len(scenarios))
            for j,impl in enumerate(impls):
                v=values[values.implementation==impl].set_index("scenario").reindex(scenarios)
                ax.bar(x+(j-(len(impls)-1)/2)*width,v['mean'],width,yerr=v.stddev,capsize=2,label=impl)
            _scale(ax,values['mean']);ax.set_title(f"{metric} / mean +/- sample stddev")
            ax.set_xticks(x,scenarios,rotation=65,ha="right",fontsize=7)
        axes[0].legend(fontsize=8);fig.tight_layout();fig.savefig(out/f"{category}_mean_stddev.png",dpi=120);plt.close(fig)
        # 所有指标、所有场景的 10/3 轮波动，按各自均值归一化，数值可回查 all_rounds.csv。
        fig, axes = _axes(metrics)
        cat_rows=all_rows[all_rows.category==category]
        for ax,metric in zip(axes,metrics):
            for (scenario,impl),g in cat_rows.groupby(["scenario","implementation"],sort=False):
                mean=g[metric].mean(); y=g[metric]/mean if mean else g[metric]
                ax.plot(g['round'],y,".-",label=f"{scenario} / {impl}",linewidth=.8)
            ax.axhline(1,color="gray",linestyle="--",linewidth=.7);ax.set_title(metric);ax.set_xlabel("round");ax.set_ylabel("value / own mean");ax.grid(alpha=.2)
        axes[0].legend(fontsize=5,loc="upper left",bbox_to_anchor=(0,1.35),ncol=2)
        fig.tight_layout();fig.savefig(out/f"{category}_round_variation.png",dpi=120,bbox_inches="tight");plt.close(fig)
    text=["# Benchmark 汇总", "",f"有效轮数：{expected_rounds}。summary.csv 使用算术均值；statistics.csv 使用样本标准差。", "",
          "均值 P99 是各轮 P99 的均值，并非合并原始样本后得到的 P99。", "",
          "柱状图显示每轮/总均值；跨轮折线按每项自身均值归一化。场景间折线没有时间序列含义。", "",
          "详细语义、工作量、配置和局限见该 suite 的 README.md 与 metadata.json。", ""]
    for category,group in means.groupby('category',sort=False):
        text += [f"## {category}", "", "| Scenario | Implementation | "+" | ".join(metrics)+" |",
                 "|---|---|"+"---:|"*len(metrics)]
        for _,r in group.iterrows():text.append(f"| {r.scenario} | {r.implementation} | "+" | ".join(f"{r[m]:.3f}" for m in metrics)+" |")
        text.append("")
    (out/"summary.md").write_text("\n".join(text),encoding="utf-8")
    (out.parent/"latest.txt").write_text(out.name+"\n",encoding="utf-8")
    return means, statistics
