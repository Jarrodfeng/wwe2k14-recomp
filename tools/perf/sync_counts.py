"""Session 20 item 3: barriers, rendering restarts, FSI EDRAM barriers, descriptor writes, pipeline changes and
dispatches per draw, from an anatomy CSV's cause columns (SDK Session 7c CAUSES; counted for each row's preparation).

    py tools\\perf\\sync_counts.py ANATOMY_CSV [--frame 0] [--group crowd=HASH,HASH ...]

Prints per-frame totals and per-draw rates for all draws, each group, and the rest.
"""
import argparse
import csv

ap = argparse.ArgumentParser()
ap.add_argument('csv')
ap.add_argument('--frame', default='0')
ap.add_argument('--group', action='append', default=[])
a = ap.parse_args()
groups = {g.split('=')[0]: set(g.split('=')[1].split(',')) for g in a.group}
COLS = ['barrier_batches', 'rendering_restarts', 'fsi_edram_barriers', 'desc_writes', 'pipeline_changed', 'dispatches',
        'tex_loads', 'up_calls']
tot = {}
kinds = {}


def add(name, r):
    t = tot.setdefault(name, {c: 0 for c in COLS} | {'draws': 0, 'with_restart': 0, 'with_barrier': 0})
    t['draws'] += 1
    for c in COLS:
        t[c] += int(r[c] or 0)
    t['with_restart'] += int(r['rendering_restarts'] or 0) > 0
    t['with_barrier'] += int(r['barrier_batches'] or 0) > 0


with open(a.csv, newline='') as f:
    for r in csv.DictReader(f):
        if r['frame'] != a.frame:
            continue
        kinds[r['kind']] = kinds.get(r['kind'], 0) + 1
        if r['kind'] != 'draw':
            add('(non-draw rows: ' + r['kind'] + ')', r)
            continue
        add('all draws', r)
        g = next((n for n, s in groups.items() if r['vs_hash'] in s), 'rest')
        add(g, r)
print('rows:', kinds)
print(f"{'group':28} {'draws':>6} " + ' '.join(f'{c[:10]:>10}' for c in COLS) + '  draws w/ restart / w/ barrier')
for n, t in tot.items():
    print(f"{n:28} {t['draws']:6} " + ' '.join(f'{t[c]:10}' for c in COLS) + f"  {t['with_restart']} / {t['with_barrier']}")
    if t['draws'] > 1:
        print(f"{'  per draw':28} {'':6} " + ' '.join(f'{t[c] / t["draws"]:10.3f}' for c in COLS))
