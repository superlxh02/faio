#!/usr/bin/env python3
"""从协程 benchmark 的 summary.csv 生成中文十轮均值报告与图表。"""
import argparse
import re
from pathlib import Path

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

METRICS = {
    'batch_ns_per_op': ('批量摊销耗时', '纳秒/操作', '越低越好'),
    'sampled_ns_per_op': ('采样摊销耗时', '纳秒/操作', '越低越好'),
    'ops_per_sec': ('批量吞吐量', '操作/秒', '越高越好'),
    'p50_ns': ('P50 延迟', '纳秒', '越低越好'),
    'p90_ns': ('P90 延迟', '纳秒', '越低越好'),
    'p99_ns': ('P99 延迟', '纳秒', '越低越好'),
    'p999_ns': ('P99.9 延迟', '纳秒', '越低越好'),
    'max_ns': ('最大延迟', '纳秒', '越低越好'),
    'migrations': ('采样模式线程迁移次数', '次', '仅供观察'),
    'batch_migrations': ('批量模式线程迁移次数', '次', '仅供观察'),
}
CATEGORIES = {
    'calibration': '计时与语言模型参考', 'switch': '协程切换与唤醒',
    'sync_contention': '同步原语争用', 'sync_composed': '组合同步',
    'sync_ready': '无争用同步', 'concurrency': '任务并发',
    'mpsc': 'MPSC 通道', 'submission': '任务提交',
}

SCENARIO_NAMES = {
    'timer_calibration': '计时器校准', 'task_await_ready': '立即完成任务等待',
    'cross_runtime_rtt': '跨运行时往返',
    'external_notification_registered': '外部线程通知已登记等待者',
    'semaphore_ready': '无争用信号量', 'mutex_ready': '无争用互斥锁',
    'barrier_ready_1': '单参与者屏障', 'latch_ready': '已打开锁存器',
    'mpsc_ready_64': 'MPSC 立即收发(c64)', 'mpsc_try_64': 'MPSC 尝试收发(c64)',
    'join_all_32': '等待全部任务(32)', 'scope_32': '任务作用域(32)',
    'join_ready_2': '等待两任务', 'select_spawn_drain_2': '选择并排空两任务',
    'block_on_entry': '阻塞入口往返',
}
SCENARIO_PREFIXES = {
    'yield': '主动让出', 'handoff_rtt': '任务交接往返',
    'mutex_contention': '互斥锁争用', 'semaphore_contention': '信号量争用',
    'barrier': '屏障争用', 'cv_roundtrip': '条件变量往返',
    'latch_fanin': '锁存器扇入', 'spawn_join': '创建并等待',
    'mpsc': 'MPSC 收发', 'external_burst': '外部批量提交',
    'internal_burst': '内部批量提交',
}


def scenario_label(scenario):
    if scenario in SCENARIO_NAMES:
        return SCENARIO_NAMES[scenario]
    for prefix, label in SCENARIO_PREFIXES.items():
        if scenario.startswith(prefix + '_'):
            suffix = scenario[len(prefix) + 1:]
            params = '/'.join(re.findall(r'(?:w|p|c|k)\d+', suffix))
            if not params and suffix.isdigit():
                params = 'w' + suffix
            return f'{label}({params})' if params else label
    return scenario


def setup_font():
    from matplotlib import font_manager
    for folder in (Path('/usr/share/fonts/opentype/noto'), Path('/usr/share/fonts/truetype/noto')):
        for font_file in folder.glob('*CJK*'):
            font_manager.fontManager.addfont(str(font_file))
    preferred = ('Noto Sans CJK SC', 'Noto Sans CJK JP', 'Source Han Sans SC', 'PingFang SC', 'Microsoft YaHei',
                 'SimHei')
    installed = {font.name for font in font_manager.fontManager.ttflist}
    match = next((font for font in preferred if font in installed), None)
    if match:
        plt.rcParams['font.sans-serif'] = [match, 'DejaVu Sans']
    else:
        print('警告：未找到中文字体；请安装 Noto Sans CJK SC 以正确显示图表标题')
    plt.rcParams['axes.unicode_minus'] = False


def fmt(value):
    return f'{value:,.2f}'


def plot_category(group, implementation, path):
    scenarios = group.scenario.drop_duplicates().tolist()
    chart_metrics = [m for m in METRICS if (group[m] != 0).any()]
    rows = (len(chart_metrics) + 1) // 2
    fig, axes = plt.subplots(rows, 2, figsize=(18, 4.6 * rows), constrained_layout=True, squeeze=False)
    x = np.arange(len(scenarios))
    for ax, metric in zip(axes.flat, chart_metrics):
        name, unit, direction = METRICS[metric]
        matrix = group.pivot(index='scenario', columns='implementation', values=metric).reindex(scenarios)
        if implementation == '对照':
            width = .38
            for offset, impl, color in [(-width / 2, 'faio', '#2876b2'), (width / 2, 'tokio', '#e88736')]:
                ax.bar(x + offset, matrix[impl], width, label=impl, color=color)
            ax.legend(fontsize=8)
        else:
            ax.bar(x, matrix[implementation], color='#2876b2' if implementation == 'faio' else '#e88736')
        ax.set_title(f'{name}（{unit}，{direction}）')
        ax.set_xticks(x, [scenario_label(s) for s in scenarios], rotation=45, ha='right', fontsize=8)
        ax.grid(axis='y', alpha=.2)
        values = matrix.to_numpy().ravel()
        positive = values[values > 0]
        if len(positive) and positive.max() / positive.min() > 20:
            ax.set_yscale('log')
            ax.set_ylabel('对数刻度')
    for ax in axes.flat[len(chart_metrics):]:
        ax.set_visible(False)
    fig.suptitle(f'{implementation} · {CATEGORIES.get(group.category.iloc[0], group.category.iloc[0])} · 十轮均值',
                 fontsize=16)
    fig.savefig(path, dpi=130)
    plt.close(fig)


def table(group, implementation):
    rows = ['| 场景 | ' + ' | '.join(v[0] for v in METRICS.values()) + ' |',
            '|---|' + '---:|' * len(METRICS)]
    for _, row in group.iterrows():
        label = f'{scenario_label(row.scenario)} (`{row.scenario}`)'
        if implementation == '对照':
            label += f' · {row.implementation}'
        rows.append('| ' + label + ' | ' + ' | '.join(fmt(row[m]) for m in METRICS) + ' |')
    return rows


def generate(result_dir):
    summary = pd.read_csv(result_dir / 'summary.csv')
    scenarios = pd.read_csv(result_dir / 'scenarios.csv')
    required = {'category', 'scenario', 'implementation', *METRICS}
    if not required.issubset(summary.columns):
        raise ValueError(f'summary.csv 缺少列：{sorted(required - set(summary.columns))}')
    if set(summary.implementation) != {'faio', 'tokio'}:
        raise ValueError('summary.csv 必须同时包含 faio 和 tokio')
    if summary.duplicated(['scenario', 'implementation']).any():
        raise ValueError('summary.csv 包含重复场景')
    if not np.isfinite(summary[list(METRICS)].to_numpy()).all():
        raise ValueError('summary.csv 包含无效数值')
    if set(summary.scenario) != set(scenarios.scenario):
        raise ValueError('场景清单与均值数据不一致')
    pairs = summary.groupby('scenario').implementation.nunique()
    if not (pairs == 2).all():
        raise ValueError('存在未配对场景')
    stats_path = result_dir / 'statistics.csv'
    if stats_path.exists():
        stats = pd.read_csv(stats_path)
        if not (stats.rounds == 10).all():
            raise ValueError('报告要求每项均为十轮均值')
    setup_font()
    chart_dir = result_dir / 'mean_report_charts'
    chart_dir.mkdir(exist_ok=True)
    lines = ['# 协程 Benchmark 十轮均值报告', '',
             '数据来源：[summary.csv](summary.csv)；场景定义：[scenarios.csv](scenarios.csv)。图表和表格均只使用十轮算术均值，不展示单轮数据。',
             '',
             '## 指标含义', '',
             '| 指标 | 单位 | 解读 |', '|---|---|---|']
    for name, unit, direction in METRICS.values():
        lines.append(f'| {name} | {unit} | {direction} |')
    lines += ['',
              '批量摊销耗时和吞吐量来自 batch 模式；采样摊销耗时、分位数和最大延迟来自 sampled 模式。吞吐量是各轮吞吐量的算术均值，不一定等于 10⁹ 除以平均批量耗时。P99 等是各轮分位数的均值，不是合并样本的分位数。',
              '',
              '横轴为测试场景，场景名称中的 w/p/c/k 分别表示 worker 数、参与者或生产者数、通道容量、许可数。分组操作的“操作/秒”实际为组/秒；具体工作量与可比性见 [协程 benchmark 协议](../../../coro/README.md)。',
              '', '## faio 十轮均值', '']
    for impl in ('faio', 'tokio', '对照'):
        if impl == 'tokio': lines += ['## Tokio 十轮均值', '']
        if impl == '对照': lines += ['## faio 与 Tokio 对照', '',
                                     '同一场景的两根柱对应同一指标。耗时和延迟越低越好，吞吐量越高越好；迁移次数仅描述线程行为。计时参考场景和代理实现不应直接视为调度器性能结论。',
                                     '']
        for category, group in summary.groupby('category', sort=False):
            name = CATEGORIES.get(category, category)
            section = group if impl == '对照' else group[group.implementation == impl]
            filename = f'{category}_{impl if impl != "对照" else "comparison"}.png'
            plot_category(section, impl, chart_dir / filename)
            lines += [f'### {name}', '', f'![{impl} {name}十轮均值](mean_report_charts/{filename})', '']
            lines += table(section, impl)
            lines.append('')
    lines += ['## 解读边界', '',
              '- 同一指标只在相同场景和操作单位下对比；不同场景的绝对值不能直接排名。',
              '- `task_await_ready` 是语言模型参考；CV、latch、scope 等代理场景仅比较对应的成功工作流。',
              '- 尾延迟和最大值可能受调度及系统抢占影响；需要判断稳定性时查阅 [statistics.csv](statistics.csv) 的标准差与变异系数。',
              '- 测试环境、CPU 亲和性和编译配置见 [metadata.json](metadata.json)。', '']
    target = result_dir / 'mean_report.md'
    target.write_text('\n'.join(lines), encoding='utf-8')
    return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('result_dir', type=Path, help='包含 summary.csv 的协程 benchmark 结果目录')
    print(generate(parser.parse_args().result_dir.resolve()))
