"""Session 20 item 10: host present-to-present times (timeline 'hpresent' events) and guest frame times in a run's
match window (scene=match +300 .. 'bench: done', as tools\\input_scripts\\short\\present_match.txt marks it).

    py tools\\perf\\present_spread.py RUN [--hz 165]
"""
import argparse
import csv
import re
from pathlib import Path

LOGS = Path(__file__).resolve().parents[2] / 'out' / 'build' / 'win-amd64-release' / 'logs'
ap = argparse.ArgumentParser()
ap.add_argument('run')
ap.add_argument('--hz', type=float, default=165.0)
a = ap.parse_args()
marks = {}
for line in open(LOGS / f'wwe2k14_{a.run}.log', encoding='utf-8', errors='replace'):
    m = re.search(r'\[input\].*?(scene=match|bench: done) \(frame (\d+)\)', line)
    if m:
        marks[m.group(1)] = int(m.group(2))
f0, f1 = marks['scene=match'] + 300, marks['bench: done']
# vdswap events carry the guest frame number: the window's time range comes from them.
t0 = t1 = None
pres = []
pres_seq = []   # Session 21: (t, refresh seq of the presented image)
refresh = []    # Session 21: (t, refresh seq) of each guest output refresh
with open(LOGS / f'timeline_{a.run}.csv', newline='') as f:
    for r in csv.DictReader(f):
        e = r['event']
        if e == 'vdswap':
            n = int(r['a'], 0)
            if n == f0:
                t0 = int(r['t_ns'])
            elif n == f1:
                t1 = int(r['t_ns'])
        elif e == 'hpresent':
            pres.append(int(r['t_ns']))
            pres_seq.append((int(r['t_ns']), int(r['a'], 0)))
        elif e == 'gorefresh':
            refresh.append((int(r['t_ns']), int(r['a'], 0)))


def spread(name, ms, budget):
    ms = sorted(ms)
    n = len(ms)
    pct = lambda p: ms[min(n - 1, int(p * n))]
    over = sum(1 for x in ms if x > 1.5 * budget)
    print(f'{name}: n={n} mean {sum(ms) / n:.3f} p1 {pct(0.01):.3f} p50 {pct(0.5):.3f} p95 {pct(0.95):.3f} '
          f'p99 {pct(0.99):.3f} max {ms[-1]:.3f} ms; > 1.5x budget ({1.5 * budget:.2f} ms): {over}')


budget = 1000.0 / a.hz
p = [x for x in pres if t0 <= x <= t1]
spread(f'host presents (frames {f0}-{f1})', [(b - c) / 1e6 for b, c in zip(p[1:], p[:-1])], budget)
with open(LOGS / f'frames_{a.run}.csv', newline='') as f:
    g = [float(r['frame_ms']) for r in csv.DictReader(f) if f0 < int(r['frame']) <= f1]
spread('guest frames (VdSwap to VdSwap)', g, budget)
print(f'duration {(t1 - t0) / 1e9:.2f} s, {len(p)} presents = {len(p) / ((t1 - t0) / 1e9):.1f} per s; '
      f'{f1 - f0} guest frames = {(f1 - f0) / ((t1 - t0) / 1e9):.1f} per s')
# Session 21: repeats (presents of an image already presented) and guest frames never shown, in the window.
shown = [q for t, q in pres_seq if t0 <= t <= t1]
made = [q for t, q in refresh if t0 <= t <= t1]
if shown and made:
    distinct = set(shown)
    lo, hi = min(made), max(made)
    never = sum(1 for q in made if q not in distinct and lo < q < hi)
    print(f'presented images: {len(shown)} presents, {len(distinct)} distinct -> {len(shown) - len(distinct)} repeats; '
          f'{len(made)} guest refreshes, {never} never shown')
