#!/usr/bin/env python3
"""解析 benchmark_matrix.sh 的原始输出，生成 Markdown 表格（多轮取中位数）。

用法: python3 benchmark_report.py <matrix 输出目录>
"""

import os
import re
import sys

LEVELS = [("low", 2, 10), ("medium", 4, 50), ("high", 4, 100),
          ("veryhigh", 8, 200), ("extreme", 8, 500)]
LEVEL_DESC = {"low": "Low", "medium": "Medium", "high": "High",
              "veryhigh": "VeryHigh", "extreme": "Extreme"}
ENDPOINTS = [("json", "GET /json", "同步"),
             ("async_status", "GET /async/status", "异步(sleep 1ms)"),
             ("api_user", "GET /api/user/42", "同步+前缀路由"),
             ("post_echo", "POST /echo", "同步+请求体回显")]


def to_ms(s):
    if not s:
        return None
    m = re.match(r'^([\d.]+)\s*(us|ms|s)$', s.strip())
    if not m:
        return None
    v = float(m.group(1))
    return v / 1000.0 if m.group(2) == 'us' else (v * 1000.0 if m.group(2) == 's' else v)


def parse_wrk(path):
    try:
        t = open(path, errors='replace').read()
    except OSError:
        return None
    r = {}

    def num(pat):
        m = re.search(pat, t)
        return float(m.group(1)) if m else None

    r['rps'] = num(r'Requests/sec:\s+([\d.]+)')
    m = re.search(r'Transfer/sec:\s+([\d.]+)(\w+)', t)
    r['transfer_mbps'] = None
    if m:
        v, u = float(m.group(1)), m.group(2)
        factor = {'KB': 1 / 1024.0, 'MB': 1.0, 'GB': 1024.0, 'B': 1 / 1048576.0}.get(u)
        r['transfer_mbps'] = v * factor if factor else None
    m = re.search(r'^\s+Latency\s+([\d.]+(?:us|ms|s))\s+([\d.]+(?:us|ms|s))\s+([\d.]+(?:us|ms|s))',
                  t, re.M)
    if m:
        r['avg'] = to_ms(m.group(1))
        r['stdev'] = to_ms(m.group(2))
        r['max'] = to_ms(m.group(3))
    for pct in (50, 75, 90, 99):
        m = re.search(r'^\s+%d%%\s+([\d.]+(?:us|ms|s))' % pct, t, re.M)
        r['p%d' % pct] = to_ms(m.group(1)) if m else None
    m = re.search(r'Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)', t)
    if m:
        r['e_connect'] = int(m.group(1))
        r['e_read'] = int(m.group(2))
        r['e_write'] = int(m.group(3))
        r['e_timeout'] = int(m.group(4))
    else:
        r.update(e_connect=0, e_read=0, e_write=0, e_timeout=0)
    m = re.search(r'Non-2xx or 3xx responses:\s+(\d+)', t)
    r['non2xx'] = int(m.group(1)) if m else 0
    m = re.search(r'(\d+) requests in', t)
    r['requests'] = int(m.group(1)) if m else None
    return r


def parse_meta(path):
    d = {}
    try:
        for line in open(path):
            line = line.strip()
            if '=' in line:
                k, v = line.split('=', 1)
                d[k] = v
    except OSError:
        return None
    return d


def load(outdir):
    data = {}
    for ep_key, _, _ in ENDPOINTS:
        for lv, _, _ in LEVELS:
            rounds = []
            for r in range(1, 7):
                txt = os.path.join(outdir, ep_key, '%s_r%d.txt' % (lv, r))
                meta = txt + '.meta'
                if not os.path.exists(meta):
                    continue
                m = parse_meta(meta) or {}
                w = parse_wrk(txt)
                if w is None:
                    continue
                w['crashed'] = int(m.get('crashed', 0))
                w['server_exit_sig'] = int(m.get('server_exit_sig', 0))
                w['server_cpu_ms'] = int(m.get('server_cpu_ms', -1))
                w['wall_ms'] = int(m.get('wall_ms', 0))
                w['round'] = r
                w['wrk_cpu_s'] = None
                try:
                    parts = open(txt + '.time').read().split()
                    if len(parts) >= 3:
                        w['wrk_cpu_s'] = float(parts[1]) + float(parts[2])
                except (OSError, ValueError):
                    pass
                rounds.append(w)
            data[(ep_key, lv)] = rounds
    return data


def median(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return None
    vals = sorted(vals)
    n = len(vals)
    return vals[n // 2] if n % 2 else (vals[n // 2 - 1] + vals[n // 2]) / 2.0


def agg(rounds, key):
    ok = [r for r in rounds if not r.get('crashed')]
    if not ok:
        return None
    return median([r.get(key) for r in ok])


def fmt(v, unit='', digits=2):
    if v is None:
        return '—'
    if digits == 0:
        return '{:,}{}'.format(int(round(v)), unit)
    return '{:,.{d}f}{}'.format(v, unit, d=digits)


def main():
    outdir = sys.argv[1]
    data = load(outdir)

    print('<!-- 吞吐 / 延迟（未崩溃轮次中位数） -->')
    print()
    print('| 端点 | 级别 | 线程/连接 | Requests/sec | 平均延迟 | P50 | P75 | P90 | P99 | 最大延迟 | 传输速率 | 有效轮/总轮 |')
    print('|------|------|-----------|--------------|----------|-----|-----|-----|-----|----------|----------|-------------|')
    for ep_key, ep_desc, _ in ENDPOINTS:
        for lv, t, c in LEVELS:
            rounds = data[(ep_key, lv)]
            n_ok = sum(1 for r in rounds if not r.get('crashed'))
            cell = '{}/{}'.format(n_ok, len(rounds)) if rounds else '—'
            print('| {} | {} | {}/{} | **{}** | {} | {} | {} | {} | {} | {} | {} | {} |'.format(
                ep_desc, LEVEL_DESC[lv], t, c,
                fmt(agg(rounds, 'rps'), '', 0),
                fmt(agg(rounds, 'avg'), 'ms', 2),
                fmt(agg(rounds, 'p50'), 'ms', 2),
                fmt(agg(rounds, 'p75'), 'ms', 2),
                fmt(agg(rounds, 'p90'), 'ms', 2),
                fmt(agg(rounds, 'p99'), 'ms', 2),
                fmt(agg(rounds, 'max'), 'ms', 2),
                fmt(agg(rounds, 'transfer_mbps'), 'MB/s', 2),
                cell))

    print()
    print('<!-- 错误 / 资源 -->')
    print()
    print('| 端点 | 级别 | Socket timeout | connect | read | write | Non-2xx | 服务器 CPU(核) | wrk CPU(核) | 崩溃轮 |')
    print('|------|------|----------------|---------|------|-------|---------|----------------|-------------|--------|')
    for ep_key, ep_desc, _ in ENDPOINTS:
        for lv, t, c in LEVELS:
            rounds = data[(ep_key, lv)]
            n_crash = sum(1 for r in rounds if r.get('crashed'))
            srv_cpu = agg(rounds, 'server_cpu_ms')
            wall = agg(rounds, 'wall_ms')
            wrk_cpu = agg(rounds, 'wrk_cpu_s')
            srv_cores = (srv_cpu / wall) if (srv_cpu is not None and wall) else None
            wrk_cores = (wrk_cpu * 1000.0 / wall) if (wrk_cpu is not None and wall) else None
            print('| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |'.format(
                ep_desc, LEVEL_DESC[lv],
                fmt(agg(rounds, 'e_timeout'), '', 0),
                fmt(agg(rounds, 'e_connect'), '', 0),
                fmt(agg(rounds, 'e_read'), '', 0),
                fmt(agg(rounds, 'e_write'), '', 0),
                fmt(agg(rounds, 'non2xx'), '', 0),
                fmt(srv_cores, '', 2),
                fmt(wrk_cores, '', 2),
                n_crash if n_crash else '0'))

    print()
    print('<!-- 逐轮明细 -->')
    print()
    print('| 端点 | 级别 | 轮 | Requests/sec | P99 | timeout | write err | 状态 |')
    print('|------|------|----|--------------|-----|---------|-----------|------|')
    for ep_key, ep_desc, _ in ENDPOINTS:
        for lv, t, c in LEVELS:
            for r in data[(ep_key, lv)]:
                st = '服务器崩溃(sig %d)' % r['server_exit_sig'] if r.get('crashed') else 'OK'
                print('| {} | {} | {} | {} | {} | {} | {} | {} |'.format(
                    ep_desc, LEVEL_DESC[lv], r['round'],
                    fmt(r.get('rps'), '', 0),
                    fmt(r.get('p99'), 'ms', 2),
                    fmt(r.get('e_timeout'), '', 0),
                    fmt(r.get('e_write'), '', 0),
                    st))


if __name__ == '__main__':
    main()
