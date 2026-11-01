"""Per passlog label (<scene>_<arm>), the median time of pass #N (and its 'before it' gap) and the frame total, from
an in-run A/B log (passab_multi routes). Pass numbers count every pass-log segment, so they shift between arms - match
a pass by its shaders instead where an arm removes one. Session 80.

    py -I tools\\native\\pass_by_arm.py LOG SCENE PASS_NUMBER
"""
import re
import statistics
import sys
from collections import defaultdict

log, scene, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
mark = re.compile(r'passlog: (\S+)')
line_re = re.compile(r'\[native\]\[pass\] frame (\d+) #(\d+) kind (\d+) (\d+x\d+) (.*?): ([0-9.]+) ms \(before it ([0-9.]+) ms\)')
label = None
times = defaultdict(list)
gaps = defaultdict(list)
shape = {}
for line in open(log, encoding='utf-8', errors='replace'):
    m = mark.search(line)
    if m:
        label = m.group(1)
        continue
    m = line_re.search(line)
    if m and label and label.startswith(scene + '_') and int(m.group(2)) == n:
        times[label].append(float(m.group(6)))
        gaps[label].append(float(m.group(7)))
        shape[label] = f'kind {m.group(3)} {m.group(4)} {m.group(5)[:60]}'
for k in sorted(times):
    print(f'{k:14s} pass #{n}: n {len(times[k]):3d} median {statistics.median(times[k]):6.3f} ms, before it '
          f'{statistics.median(gaps[k]):6.3f} ms  ({shape[k]})')
