"""Session 80 (frame structure): one pass-log frame's deferred batches (gpu_ram_stats) - kind 9, the barriers into the
read layouts, and kind 11, each dispatch, summed by what the dispatch does: a depth or colour source, writing guest
memory or not, filling a texture or not; the ones over 0.3 ms listed. Input as tools\\native\\pass_gaps.py.

    py -I tools\\native\\pass_dispatches.py f.txt
"""
import re
import sys
from collections import defaultdict

rx = re.compile(r'#(\d+) kind (\d+) (\d+)x(\d+) pitch \d+ colour kind (\d+) depth (\d+) draws (\d+) vs (\w+) ps (\w+): '
                r'([0-9.]+) ms')
raw = open(sys.argv[1], 'rb').read()
text = raw.decode('utf-16') if raw[:2] == b'\xff\xfe' else raw.decode('utf-8', 'replace')
groups = defaultdict(lambda: [0, 0.0, 0])
barriers = [0, 0.0]
big = []
for line in text.splitlines():
    m = rx.search(line)
    if not m:
        continue
    kind, ms = int(m[2]), float(m[10])
    if kind == 9:
        barriers[0] += 1
        barriers[1] += ms
        if ms >= 0.3:
            big.append(f'  #{m[1]} barriers (entries {m[7]}, depth {m[6]}, memory {m[5]}): {ms:.2f} ms')
    elif kind == 11:
        key = ('depth' if m[6] == '1' else 'colour') + (' -> memory' if m[5] == '1' else '') + \
              (' + texture' if int(m[9], 16) else '')
        g = groups[key]
        g[0] += 1
        g[1] += ms
        g[2] += int(m[7])
        if ms >= 0.3:
            big.append(f'  #{m[1]} dispatch {key} {m[3]}x{m[4]} mem {int(m[7]) / 1e6:.2f} MB fmt {int(m[8], 16)}: '
                       f'{ms:.2f} ms')
print(f'barriers into read layouts: {barriers[0]} batches, {barriers[1]:.2f} ms')
for k, (n, ms, b) in sorted(groups.items(), key=lambda kv: -kv[1][1]):
    print(f'dispatches {k}: {n}, {ms:.2f} ms, {b / 1e6:.1f} MB to memory')
print('\n'.join(big))
