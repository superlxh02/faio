#!/usr/bin/env python3
"""faio / Tokio：10 轮，两种计时模式；先串行测量，再生成全部 CSV/图表。"""
import argparse
import gzip
import hashlib
import io
import json
import os
import resource
import shutil
import time
from pathlib import Path

import numpy as np
import pandas as pd
from benchmark_report import (benchmark_lock, command, configure_cpp, create_output, latency_curves,
                              metadata, round_report, summary_report)

METRICS = ["batch_ns_per_op", "sampled_ns_per_op", "ops_per_sec", "p50_ns", "p90_ns", "p99_ns",
           "p999_ns", "max_ns", "migrations", "batch_migrations"]


def scenario_definitions():
    """显式场景清单，防止两边同时漏掉一个测试却被误判为完整对比。"""
    rows = []
    def add(name, category, workers=1, unit="operation", comparison="paired", **extra):
        rows.append({"scenario":name,"category":category,"workers":workers,"unit":unit,
                     "comparison":comparison,**extra})
    add("timer_calibration","calibration",0,comparison="reference")
    for w in [1,4]:
        add(f"yield_{w}","switch",w)
        add(f"handoff_rtt_w{w}","switch",w,"roundtrip")
        add(f"mutex_contention_p4_w{w}","sync_contention",w,participants=4)
        add(f"semaphore_contention_k2_p4_w{w}","sync_contention",w,participants=4,permits=2)
        add(f"barrier_p4_w{w}","sync_contention",w,"arrival",participants=4)
        add(f"cv_roundtrip_w{w}","sync_composed",w,"roundtrip",comparison="tokio_notify_proxy")
        add(f"latch_fanin_32_w{w}","sync_composed",w,"group_of_32",comparison="tokio_counter_notify_proxy")
        add(f"spawn_join_{w}","concurrency",w)
        for c in [64,1024]:
            for p in [1,4]:add(f"mpsc_p{p}_w{w}_c{c}","mpsc",w,"message",producers=p,capacity=c,message_bytes=16)
        for origin in ["external","internal"]:add(f"{origin}_burst_{w}","submission",w,"task")
    for name in ["semaphore_ready","mutex_ready","barrier_ready_1"]:add(name,"sync_ready")
    add("latch_ready","sync_composed",comparison="tokio_counter_proxy")
    for name in ["mpsc_ready_64","mpsc_try_64"]:add(name,"mpsc",capacity=64,message_bytes=8,unit="send_receive_pair")
    add("task_await_ready","calibration",comparison="language_model_reference")
    for name,unit in [("join_all_32","group_of_32"),("scope_32","group_of_32"),("join_ready_2","group_of_2"),("select_spawn_drain_2","group_of_2")]:
        add(name,"concurrency",unit=unit,comparison="joinset_proxy" if name=="scope_32" else "paired")
    add("cross_runtime_rtt","switch",2,"roundtrip",runtime_count=2)
    add("external_notification_registered","switch",1,"notification",helper_threads=1)
    add("block_on_entry","submission",1,"host_worker_host_roundtrip")
    return pd.DataFrame(rows)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo",default=str(Path(__file__).resolve().parent.parent))
    p.add_argument("--count",type=int,default=100000)
    p.add_argument("--cpus",default="0-5",help="同一 CPU 集合供两边使用，跨 runtime 仍保证独立线程")
    p.add_argument("--output",help="指定一个尚不存在的输出目录")
    args=p.parse_args()
    if args.count<1024 or args.count%4: p.error("count 必须 >=1024 且能被 4 整除")
    repo=Path(args.repo).resolve();rounds=10
    with benchmark_lock(repo):
        out=create_output(repo,"coro",args.output);build=configure_cpp(repo)
        print("[build] GCC -O2 / Tokio opt-level=2",flush=True)
        command(["cmake","--build",str(build),"-j4","--target","faio_coro_benchmark"],repo)
        rust_dir=repo/"benchmark/coro/tokio-benchmark"
        rust_target=repo/"build/benchmark-rust/coro"
        command(["cargo","build","--release","--locked","--target-dir",str(rust_target)],rust_dir)
        bins={"faio":build/"benchmark/faio_coro_benchmark","tokio":rust_target/"release/tokio-coroutine-comparison"}
        definitions=scenario_definitions();definitions.to_csv(out/"scenarios.csv",index=False)
        metadata(repo,out,args,{"suite":"coro","rounds":rounds,"warmup":"full suite at count=min(count,1024) in each process",
            "sampling_modes":["batch (no per-operation clock reads)","sampled (all samples recorded)"],
            "tokio_version":"1.49.0","faio_idle_spin_count":32,
            "binary_sha256":{k:hashlib.sha256(v.read_bytes()).hexdigest() for k,v in bins.items()},
            "cargo_lock_sha256":hashlib.sha256((rust_dir/"Cargo.lock").read_bytes()).hexdigest()})
        commands=[];frames=[];usage=[]
        for number in range(1,rounds+1):
            rd=out/f"round_{number:02}";rd.mkdir()
            by_impl={};order=list(bins) if number%2 else list(reversed(bins))
            for impl in order:
                modes=["batch","sampled"] if number%2 else ["sampled","batch"]
                measured={}
                for mode in modes:
                    samples=rd/"samples"/impl
                    cmd=["taskset","-c",args.cpus,str(bins[impl]),str(args.count),mode]
                    if mode=="sampled":cmd.append(str(samples))
                    print(f"[measure] round {number}/10 / {impl} / {mode}",flush=True)
                    before=resource.getrusage(resource.RUSAGE_CHILDREN);started=time.monotonic()
                    result=command(cmd,repo,timeout=300)
                    wall=time.monotonic()-started;after=resource.getrusage(resource.RUSAGE_CHILDREN)
                    usage.append({"round":number,"implementation":impl,"mode":mode,"wall_seconds":wall,
                        "user_seconds":after.ru_utime-before.ru_utime,"system_seconds":after.ru_stime-before.ru_stime,
                        "cpu_percent":100*((after.ru_utime-before.ru_utime)+(after.ru_stime-before.ru_stime))/wall})
                    commands.append({"round":number,"implementation":impl,"mode":mode,"argv":cmd})
                    (rd/f"{impl}_{mode}.csv").write_text(result.stdout,encoding="utf-8")
                    (rd/f"{impl}_{mode}.stderr.log").write_text(result.stderr,encoding="utf-8")
                    data=pd.read_csv(io.StringIO(result.stdout))
                    if data.scenario.duplicated().any() or set(data.scenario)!=set(definitions.scenario):raise RuntimeError(f"{impl}/{mode}: 场景不完整")
                    if not np.isfinite(data.drop(columns='scenario').to_numpy()).all():raise RuntimeError("无效指标")
                    if (data.operations<=0).any() or (data.ns_per_op<=0).any():raise RuntimeError("工作量/整段耗时必须为正数")
                    if mode=="sampled" and (data["max_ns"]<data.p999_ns).any():raise RuntimeError("分位数错误")
                    measured[mode]=data.set_index('scenario')
                sampled=measured['sampled'].copy();batch=measured['batch']
                if not sampled.operations.equals(batch.operations):raise RuntimeError("两种计时模式的工作量不同")
                sampled=sampled.rename(columns={"ns_per_op":"sampled_ns_per_op"})
                sampled['batch_ns_per_op']=batch.ns_per_op;sampled['batch_migrations']=batch.migrations
                sampled['ops_per_sec']=1e9/batch.ns_per_op;sampled['implementation']=impl;sampled['round']=number
                by_impl[impl]=sampled.reset_index()
            if not by_impl['faio'].set_index('scenario').operations.equals(by_impl['tokio'].set_index('scenario').operations):raise RuntimeError("两边工作量不同")
            frame=pd.concat(list(by_impl.values())).merge(definitions,on='scenario',validate='many_to_one')
            frame.to_csv(rd/'comparison.csv',index=False,float_format='%.6f');frames.append(frame)
        # 计时结束后才压缩、绘图，以免 Python 生成图表占用 CPU 干扰下一轮。
        (out/'commands.json').write_text(json.dumps(commands,indent=2),encoding='utf-8')
        pd.DataFrame(usage).to_csv(out/'process_usage.csv',index=False)
        for number,frame in enumerate(frames,1):
            rd=out/f"round_{number:02}";print(f"[report] round {number}/10",flush=True)
            for file in (rd/'samples').glob('*/*.csv'):
                # 原始数据完整保留；gzip 不改变样本的顺序和精度。
                with file.open('rb') as source,gzip.open(str(file)+'.gz','wb',compresslevel=6) as destination:shutil.copyfileobj(source,destination)
                file.unlink()
            round_report(frame,rd,METRICS);latency_curves(rd/'samples',rd)
        means,stats=summary_report(frames,out,METRICS,rounds)
        # 比率用两边 10 轮算术均值计算；<1 表示 faio 的该延迟/开销更小。
        comparisons=[]
        for metric in ["batch_ns_per_op","sampled_ns_per_op","p50_ns","p99_ns","p999_ns"]:
            matrix=means.pivot(index='scenario',columns='implementation',values=metric)
            for name,row in matrix.iterrows():comparisons.append({"scenario":name,"metric":metric,"faio":row.faio,"tokio":row.tokio,
                "faio_over_tokio":row.faio/row.tokio if row.tokio else None})
        pd.DataFrame(comparisons).merge(definitions,on='scenario').to_csv(out/'ratios.csv',index=False)
        print(f"[done] {out}",flush=True)


if __name__=='__main__':main()
