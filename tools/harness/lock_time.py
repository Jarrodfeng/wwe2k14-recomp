"""Run-lock hold time since a start time, by kind (build / game run), from out\\harness\\runlock\\history.tsv."""
import sys
from datetime import datetime
start = datetime.fromisoformat(sys.argv[1])
rows = []
with open(r'C:\Users\Jarrod\wwe2k14-recomp\out\harness\runlock\history.tsv', encoding='utf-8', errors='replace') as f:
    for line in f:
        c = line.rstrip('\n').split('\t')
        if len(c) < 8 or c[1] != 'release':
            continue
        try:
            t = datetime.fromisoformat(c[0])
            held = float(c[7])
        except ValueError:
            continue
        if t < start:
            continue
        rows.append((c[2], c[4], held))
tot = {}
for mode, name, held in rows:
    n = name.lower()
    kind = ('build' if ('build' in n and 'run_list' not in n) else
            'run (exclusive)' if mode == 'exclusive' else 'run (shared lane)')
    tot.setdefault(kind, [0.0, 0])
    tot[kind][0] += held
    tot[kind][1] += 1
for k, (s, n) in sorted(tot.items()):
    print(f'{k:20s} {n:4d} holds  {s / 3600:6.2f} h lock time')
