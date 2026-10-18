"""Session 20 item 2: vertex shaders of one anatomy frame, for building skip lists by category.

    py tools\\perf\\vs_groups.py ANATOMY_CSV [--frame 0] [--chars tools\\matte\\character_vs.txt]

One line per vertex shader: draws, first / last draw index, primitive types, colour target(s) (rb_color_info0),
whether the draw has extents, and whether it is in the character list (the matte mask's set).
"""
import argparse
import collections
import csv

ap = argparse.ArgumentParser()
ap.add_argument('csv')
ap.add_argument('--frame', default='0')
ap.add_argument('--chars', default='tools/matte/character_vs.txt')
a = ap.parse_args()
chars = set(open(a.chars).read().strip().split(','))
g = collections.OrderedDict()
order = 0
with open(a.csv, newline='') as f:
    for r in csv.DictReader(f):
        if r['frame'] != a.frame or r['kind'] != 'draw':
            continue
        order += 1
        v = g.setdefault(r['vs_hash'], {'n': 0, 'first': order, 'last': order, 'prim': set(), 'rt': set(), 'ps': set(), 'cp': 0.0})
        v['n'] += 1
        v['last'] = order
        v['prim'].add(r['prim'])
        v['rt'].add(r['rb_color_info0'])
        v['ps'].add(r['ps_hash'])
        v['cp'] += float(r['cp_us'] or 0)
print(f'{order} draws, {len(g)} vertex shaders')
for h, v in sorted(g.items(), key=lambda kv: kv[1]['first']):
    print(f"{h} {'CHAR' if h in chars else '    '} n={v['n']:5} draws {v['first']:5}-{v['last']:5} prim={','.join(sorted(v['prim']))}"
          f" ps={len(v['ps'])} rt={','.join(sorted(v['rt']))[:60]} cp={v['cp'] / 1000:.2f}ms")
