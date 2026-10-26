r"""Compare [resolve_stats] hashes of two runs by destination: the n-th resolve into each destination in run A vs the
n-th in run B (frame numbers may shift between runs). Prints per destination: count A / B, same hashes, first
difference (frame, size, surface_info, colour_info). Wide (attire-sized) destinations first.

    py -I tools\native\resolve_compare_by_dest.py RUN_A RUN_B   (Session 69)
"""
import os
import re
import sys
from collections import defaultdict

LOGS = r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs'
LINE = re.compile(r'\[resolve_stats\] resolve \d+ dest 0x([0-9A-F]+) (\d+) bytes: non-zero bytes ([0-9.]+)%.*'
                  r'color_info 0x([0-9A-F]+) surface_info 0x([0-9A-F]+), hash ([0-9A-F]+), frame (\d+)')


def load(run):
    per = defaultdict(list)
    with open(os.path.join(LOGS, f'wwe2k14_{run}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = LINE.search(line)
            if m:
                d, size, nz, ci, si, h, fr = m.groups()
                per[d].append((h, int(size), si, ci, int(fr), float(nz)))
    return per


a, b = load(sys.argv[1]), load(sys.argv[2])
print(f'destinations: A {len(a)}, B {len(b)}; resolves A {sum(map(len, a.values()))}, B {sum(map(len, b.values()))}')
rows = []
for d in sorted(set(a) | set(b)):
    la, lb = a.get(d, []), b.get(d, [])
    same = sum(1 for x, y in zip(la, lb) if x[0] == y[0])
    first = next(((x, y) for x, y in zip(la, lb) if x[0] != y[0]), None)
    size = (la or lb)[0][1]
    rows.append((d, len(la), len(lb), same, first, size, (la or lb)[0][2]))
rows.sort(key=lambda r: (r[3] == min(r[1], r[2]), -r[5]))
for d, na, nb, same, first, size, si in rows:
    tag = 'SAME' if first is None and na == nb else 'DIFF'
    line = f'{tag} dest 0x{d} size {size} surface_info 0x{si}: A {na} B {nb}, same {same}'
    if first:
        x, y = first
        line += f'; first diff A frame {x[4]} nz {x[5]:.1f}% vs B frame {y[4]} nz {y[5]:.1f}%'
    print(line)
