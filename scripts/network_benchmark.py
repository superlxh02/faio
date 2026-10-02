#!/usr/bin/env python3
"""TCP 三轮 wrk 测试，使用固定 HTTP 报文模拟请求/响应负载。"""
import argparse
import os
import re
import signal
import socket
import subprocess
import time
import urllib.error
import urllib.request
from pathlib import Path

import pandas as pd
from benchmark_report import benchmark_lock, command, configure_cpp, create_output, metadata, round_report, summary_report

METRICS=["requests_per_sec","latency_avg_ms","latency_p50_ms","latency_p90_ms","latency_p99_ms",
         "timeout_count","connect_errors","read_errors","write_errors","non_success_count","transfer_bytes_per_sec"]
BODY=b"hello benchmark\n"


def parse_wrk(output,implementation):
    """wrk 自带直方图只输出这些分位数；完整原始 stdout 每轮归档，不伪造逐请求样本。"""
    def value(pattern):
        m=re.search(pattern,output,re.MULTILINE)
        if not m:raise RuntimeError(f"{implementation}: 无法解析 wrk 输出\n{output}")
        return m
    def ms(number,unit):return float(number)*{'us':.001,'ms':1,'s':1000}[unit]
    row={"implementation":implementation,"scenario":"keep_alive_get_index","category":"network"}
    row['requests_per_sec']=float(value(r"Requests/sec:\s+([0-9.]+)")[1])
    avg=value(r"Latency\s+([0-9.]+)\s*(ms|us|s)")
    row['latency_avg_ms']=ms(avg[1],avg[2])
    for percentile in [50,90,99]:
        p=value(rf"^\s*{percentile}(?:\.0+)?%\s+([0-9.]+)\s*(us|ms|s)\s*$")
        row[f'latency_p{percentile}_ms']=ms(p[1],p[2])
    transfer=value(r"Transfer/sec:\s+([0-9.]+)\s*([KMGTP]?B)")
    row['transfer_bytes_per_sec']=float(transfer[1])*1024**['B','KB','MB','GB','TB','PB'].index(transfer[2])
    errors=re.search(r"Socket errors:\s+connect\s+(\d+),\s+read\s+(\d+),\s+write\s+(\d+),\s+timeout\s+(\d+)",output)
    for i,key in enumerate(['connect_errors','read_errors','write_errors','timeout_count']):row[key]=int(errors[i+1]) if errors else 0
    non_success=re.search(r'Non-2xx or 3xx responses:\s+(\d+)',output)
    row['non_success_count']=int(non_success[1]) if non_success else 0
    return row


def stop_process(proc):
    """只关闭本脚本启动的进程组；不杀占用端口的其他用户程序。"""
    if proc is None or proc.poll() is not None:return
    os.killpg(proc.pid,signal.SIGTERM)
    try:proc.wait(timeout=4)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid,signal.SIGKILL);proc.wait(timeout=4)


def wait_ready(proc,port):
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        if proc.poll() is not None:raise RuntimeError(f"server exited, code={proc.returncode}; see server log")
        try:
            with urllib.request.urlopen(f'http://127.0.0.1:{port}/index',timeout=1) as response:
                if response.status!=200 or response.read()!=BODY:raise RuntimeError('服务响应正文/状态不一致')
            return
        except (urllib.error.URLError,TimeoutError):time.sleep(.1)
    raise RuntimeError('服务启动超时')


def run():
    suite = 'tcp'
    p=argparse.ArgumentParser(description=f"{suite}: 3 rounds, arithmetic mean, CSV + Python bar/line charts")
    p.add_argument('--repo',default=str(Path(__file__).resolve().parent.parent))
    p.add_argument('--wrk',default='wrk');p.add_argument('--threads',type=int,default=4)
    p.add_argument('--connections',type=int,default=5000);p.add_argument('--duration',default='60s')
    p.add_argument('--warmup',default='3s');p.add_argument('--server-workers',type=int,default=4)
    p.add_argument('--output',help='指定尚不存在的结果目录');args=p.parse_args()
    if min(args.threads,args.connections,args.server_workers)<1 or args.connections<args.threads:p.error('无效线程/连接数')
    repo=Path(args.repo).resolve();rounds=3
    with benchmark_lock(repo):
        out=create_output(repo,suite,args.output);build=configure_cpp(repo)
        cpp=[('asio','asio_tcp_benchmark',10090),('faio','faio_tcp_benchmark',18081)]
        rust=repo/'benchmark/tcp/tokio-benchmark';print('[build] TCP',flush=True)
        rust_target=repo/'build/benchmark-rust/tcp'
        command(['cargo','build','--release','--locked','--target-dir',str(rust_target)],rust)
        targets=[(name,[str(build/'benchmark'/target)],port) for name,target,port in cpp]
        targets.append(('tokio',[str(rust_target/'release/tokio-tcp-benchmark')],10092))
        command(['cmake','--build',str(build),'-j4','--target',*[target for _,target,_ in cpp]],repo)
        metadata(repo,out,args,{'suite':suite,'rounds':rounds,'workload':'HTTP/1.1 GET /index keep-alive, 16-byte identical body',
            'note':'TCP suite implements minimal HTTP framing over TCP. wrk is a closed-loop generator on localhost.',
            'targets':targets,'cpp_workers':args.server_workers})
        frames=[];commands=[]
        for number in range(1,rounds+1):
            rd=out/f'round_{number:02}';rd.mkdir();rows=[]
            # 三个实现三轮各在首/中/尾运行一次，降低固定顺序带来的偏差。
            order=targets[(number-1)%3:]+targets[:(number-1)%3]
            for name,argv,port in order:
                print(f'[measure] {suite} round {number}/3 / {name}',flush=True)
                # 只检查端口，遇到占用就失败，避免测到已有服务。
                with socket.socket() as check:
                    check.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);check.bind(('127.0.0.1',port))
                server_cmd=[*argv,'0.0.0.0',str(port),str(args.server_workers)]
                proc=None
                try:
                    with (rd/f'{name}_server.log').open('w') as log:
                        proc=subprocess.Popen(server_cmd,cwd=repo,start_new_session=True,stdout=log,stderr=subprocess.STDOUT)
                        wait_ready(proc,port)
                        for mode,duration in [('warmup',args.warmup),('measured',args.duration)]:
                            wrk_cmd=[args.wrk,'--latency',f'-t{args.threads}',f'-c{args.connections}',f'-d{duration}',f'http://127.0.0.1:{port}/index']
                            completed=command(wrk_cmd,repo,timeout=600)
                            (rd/f'{name}_{mode}.txt').write_text(completed.stdout+completed.stderr,encoding='utf-8')
                            commands.append({'round':number,'implementation':name,'server':server_cmd,'mode':mode,'wrk':wrk_cmd})
                            if proc.poll() is not None:raise RuntimeError(f'{name}: 服务在测试中退出')
                            if mode=='measured':row=parse_wrk(completed.stdout,name);row['round']=number;rows.append(row)
                finally:stop_process(proc)
            frame=pd.DataFrame(rows);frame.to_csv(rd/'comparison.csv',index=False);frames.append(frame)
        (out/'commands.json').write_text(__import__('json').dumps(commands,indent=2),encoding='utf-8')
        for number,frame in enumerate(frames,1):round_report(frame,out/f'round_{number:02}',METRICS)
        summary_report(frames,out,METRICS,rounds)
        print(f'[done] {out}',flush=True)
