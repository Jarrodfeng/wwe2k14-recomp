"""Session 53 item 2.7: bursts of failed guest physical-memory allocations (BaseHeap / PhysicalHeap errors and the
[wwe2k14][mem] MmAllocatePhysicalMemoryEx lines) per 100 ms, next to the long frames of the same run.

    py tools\\perf\\s53_alloc_bursts.py RUN [--min-lines 50] [--logs DIR]
"""
import argparse
import collections
import csv
import datetime as dt
import os
import re

ap = argparse.ArgumentParser()
ap.add_argument('run')
ap.add_argument('--min-lines', type=int, default=50)
ap.add_argument('--logs', default=r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs')
a = ap.parse_args()
TS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]')
anchor = None
bins = collections.Counter()
for ln in open(os.path.join(a.logs, f'wwe2k14_{a.run}.log'), encoding='utf-8', errors='replace'):
    m = TS.match(ln)
    if not m:
        continue
    t = dt.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f')
    if anchor is None and '[frames] first guest frame' in ln:
        anchor = t
    if anchor and ('BaseHeap::Alloc' in ln or 'PhysicalHeap::Alloc' in ln or 'MmAllocatePhysicalMemoryEx' in ln):
        bins[int((t - anchor).total_seconds() * 10)] += 1
frames = []
with open(os.path.join(a.logs, f'frames_{a.run}.csv')) as f:
    for r in csv.DictReader(f):
        frames.append((float(r['t_seconds']), float(r['frame_ms'])))
print(f'run {a.run}: {sum(bins.values())} allocation-failure lines after the first frame')
for b in sorted(bins):
    if bins[b] < a.min_lines:
        continue
    t0, t1 = b / 10, b / 10 + 0.1
    worst = max([ms for t, ms in frames if t0 - 0.05 <= t <= t1 + 0.15] or [0])
    print(f'  t={t0:7.1f}-{t1:5.1f}s  {bins[b]:5d} lines   worst frame nearby {worst:6.1f} ms')
