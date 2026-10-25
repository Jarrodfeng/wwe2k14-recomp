"""Summary of a phone run's load samples (tools/android/sample.ps1 -> out/android/runs/NNN/samples.txt).

    py tools/android/samples_report.py 025 [--from S] [--to S] [--top 12]

Per thread: mean / max CPU % over the samples (100 % = one core busy), the cores it ran on, and the thread's role from
the game's log (the [tNNNNN] tag of its log lines: gpu = GPU command thread, ...). Then the CPU clocks, the thermal status
and the hottest CPU / GPU / skin temperatures over time (one line per ~30 s). --from / --to: seconds after the first sample.
"""
import argparse
import glob
import os
import re
import statistics
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def thread_roles(run_dir):
    """tid -> the log categories its lines carry (newest game log of the run)."""
    logs = sorted(glob.glob(os.path.join(run_dir, 'logs', 'wwe2k14_*.log')), key=os.path.getmtime)
    roles = defaultdict(Counter)
    if not logs:
        return {}
    with open(logs[-1], encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.match(r'\[[^\]]+\] \[\w+\] \[(\w+)\] \[t(\d+)\]', line)
            if m:
                roles[int(m.group(2))][m.group(1)] += 1
    return {t: ','.join(k for k, _ in c.most_common(2)) for t, c in roles.items()}


def parse(path):
    samples = []
    cur = None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip()
        if line.startswith('===='):
            cur = {'t': float(line.split()[1]), 'threads': {}, 'temps': {}, 'freq': None, 'status': None}
            samples.append(cur)
            continue
        if cur is None:
            continue
        m = re.match(r'\s*(\d+)\s+([\d.]+)\s+(\d+)\s+(.*)$', line)
        if m:
            cur['threads'][int(m.group(1))] = (float(m.group(2)), int(m.group(3)), m.group(4).strip())
            continue
        m = re.match(r'freq (\d+) (\d+)', line)
        if m:
            cur['freq'] = (int(m.group(1)) // 1000, int(m.group(2)) // 1000)
            continue
        m = re.match(r'Thermal Status: (\d+)', line)
        if m:
            cur['status'] = int(m.group(1))
            continue
        m = re.search(r'mValue=([\d.]+), mType=\d+, mName=(\w+),', line)
        if m:
            cur['temps'][m.group(2)] = float(m.group(1))
    return samples


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--from', dest='t_from', type=float, default=0)
    ap.add_argument('--to', dest='t_to', type=float, default=1e9)
    ap.add_argument('--top', type=int, default=12)
    a = ap.parse_args()
    run_dir = os.path.join(ROOT, 'out', 'android', 'runs', a.run)
    samples = parse(os.path.join(run_dir, 'samples.txt'))
    if not samples:
        sys.exit('no samples')
    t0 = samples[0]['t']
    samples = [s for s in samples if a.t_from <= s['t'] - t0 <= a.t_to]
    roles = thread_roles(run_dir)
    n = len(samples)
    print(f'{n} samples over {samples[-1]["t"] - samples[0]["t"]:.0f} s')
    per = defaultdict(list)
    cores = defaultdict(Counter)
    names = {}
    for s in samples:
        for tid, (cpu, core, name) in s['threads'].items():
            per[tid].append(cpu)
            cores[tid][core] += 1
            names[tid] = name
    rows = sorted(per, key=lambda t: -sum(per[t]) / n)[:a.top]
    print(f'{"tid":>6} {"name":<18} {"role":<12} {"mean%":>6} {"max%":>6} {">90%":>5}  cores')
    for tid in rows:
        v = per[tid] + [0.0] * (n - len(per[tid]))  # absent from a sample's top list = below the cut
        busy = sum(1 for x in v if x >= 90)
        cs = ' '.join(f'{c}:{k}' for c, k in sorted(cores[tid].items()))
        print(f'{tid:>6} {names[tid][:18]:<18} {roles.get(tid, ""):<12} {statistics.mean(v):6.1f} {max(v):6.1f} '
              f'{busy:5d}  {cs}')
    print('\n  t(s)  status  little/big MHz  CPU max  GPU max  skin  socd')
    step = max(1, n // 20)
    for s in samples[::step]:
        tc = [v for k, v in s['temps'].items() if k.startswith('CPU')]
        tg = [v for k, v in s['temps'].items() if k.startswith('GPU')]
        fr = s['freq'] or (0, 0)
        print(f'{s["t"] - t0:6.0f}  {s["status"]!s:>6}  {fr[0]:5d}/{fr[1]:5d}    {max(tc, default=0):6.1f}  '
              f'{max(tg, default=0):6.1f}  {s["temps"].get("skin", 0):4.1f}  {s["temps"].get("socd", 0):4.1f}')


if __name__ == '__main__':
    main()
