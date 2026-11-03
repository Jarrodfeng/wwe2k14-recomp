"""Profile stacks over a guest frame range, one thread: stack_range.py STACKS FIRST LAST [THREAD] [TOP]
Prints samples per frame by leaf (self) and inclusive, plus a wait / driver / ours split by leaf module."""
import re, sys
from collections import Counter
path, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
thread = sys.argv[4] if len(sys.argv) > 4 else 'GPU Commands'
top = int(sys.argv[5]) if len(sys.argv) > 5 else 25
names, stacks, hits = {}, {}, Counter()
with open(path, encoding='utf-8', errors='replace') as f:
    for line in f:
        if line.startswith('n '):
            _, nid, name = line.rstrip('\n').split(' ', 2)
            names[int(nid)] = name
        elif line.startswith('s '):
            _, sid, key = line.rstrip('\n').split(' ', 2)
            parts = key.split(',')
            stacks[int(sid)] = (parts[0], [int(x) for x in parts[1:] if x])
        elif line and line[0].isdigit():
            fr, sid = line.split(' ')[:2]
            if a <= int(fr) <= b:
                hits[int(sid)] += 1
frames = b - a + 1
self_c, incl_c, cat = Counter(), Counter(), Counter()
WAIT = re.compile(r'Wait|Sleep|KeDelay|ConditionVariable|NtDelay|SwitchToThread|YieldProcessor|NtYield')
total = 0
for sid, n in hits.items():
    th, ids = stacks[sid]
    if th != thread:
        continue
    total += n
    fn = [names.get(i, '?') for i in ids]
    self_c[fn[0]] += n
    for x in set(fn):
        incl_c[x] += n
    leaf = fn[0]
    if any(WAIT.search(x) for x in fn[:6]):
        cat['wait / sleep (in the top 6 frames)'] += n
    elif leaf.lower().startswith(('igvk', 'igc', 'igd', 'win32u', 'dxcore', 'gdi32', 'nvoglv', 'nvgpu')):
        cat['driver / kernel graphics'] += n
    elif leaf.startswith(('rexgpu', 'rexruntime', 'rexui', 'wwe2k14')):
        cat['ours (rexgpu / runtime)'] += n
    else:
        cat['other (' + leaf.split('!')[0] + ')'] += n
print(f'thread {thread!r}, frames {a}-{b} ({frames}), samples {total} = {total / frames:.2f} per frame')
for k, v in cat.most_common(12):
    print(f'  {100 * v / total:5.1f}%  {k}')
print('\nself (leaf), share of samples:')
for k, v in self_c.most_common(top):
    print(f'  {100 * v / total:5.1f}%  {k[:140]}')
print('\ninclusive, share of samples:')
for k, v in incl_c.most_common(top + 15):
    print(f'  {100 * v / total:5.1f}%  {k[:140]}')
