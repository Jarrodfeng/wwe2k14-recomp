"""Session 80 (2026-11-02 09:45, Surface run 033): an in-run A/B log's frames (passab_multi routes) split into the two
frame-time states seen on the Surface (a fast and a slow state, ~3.5 ms apart in the match, that flip independently of
the arms, so plain medians mostly show how many frames each arm caught in each state): per arm, each state's frame
count and median, and the difference to the base arm within each state.
    py -I tools\\native\\passab_states.py LOG SCENE BASE_ARM [THRESHOLD_MS]
(threshold default: midway between the 20th and 80th percentile of all the scene's frames)"""
import re
import statistics
import sys
from collections import defaultdict

log, scene, base = sys.argv[1], sys.argv[2], sys.argv[3]
mark = re.compile(r'passlog: (\S+)')
line_re = re.compile(r'\[native\]\[pass\] frame (\d+) #\d+ .*?: ([0-9.]+) ms \(before it ([0-9.]+) ms\)')
label = None
tot = defaultdict(lambda: defaultdict(float))
for line in open(log, encoding='utf-8', errors='replace'):
    m = mark.search(line)
    if m:
        label = m.group(1)
        continue
    m = line_re.search(line)
    if m and label and label.startswith(scene + '_'):
        tot[label][m.group(1)] += float(m.group(2)) + float(m.group(3))
allv = sorted(v for lab in tot for v in tot[lab].values())
if len(sys.argv) > 4:
    thr = float(sys.argv[4])
else:
    thr = (allv[int(len(allv) * 0.2)] + allv[int(len(allv) * 0.8)]) / 2
print(f'{scene}: threshold {thr:.2f} ms')
res = {}
for lab in sorted(tot):
    v = list(tot[lab].values())
    lo = [x for x in v if x < thr]
    hi = [x for x in v if x >= thr]
    res[lab] = (statistics.median(lo) if lo else None, statistics.median(hi) if hi else None)
    print(f'  {lab:16s} fast {len(lo):2d} frames median {res[lab][0] or 0:6.2f}   slow {len(hi):2d} frames median '
          f'{res[lab][1] or 0:6.2f}')
b = res.get(f'{scene}_{base}')
if b:
    for lab in sorted(res):
        d = [(res[lab][i] - b[i]) / b[i] * 100 if res[lab][i] and b[i] else float('nan') for i in (0, 1)]
        print(f'  {lab:16s} vs {base}: fast {d[0]:+5.1f}%  slow {d[1]:+5.1f}%')
