r"""Draws per shader pair inside one pass of a frame-anatomy CSV (Session 89; promoted from Session 87's
pass_shaders.py): draws, command-processor us, the commonest (primitive, index count) per pair, and runs of
identical-state draws (what a batching / instancing step could merge).

    py -I tools\native\pass_shaders.py ANATOMY.csv [--pass N | --biggest] [--list] [--top 14]

ANATOMY.csv = logs\anatomy_RUN_K.csv (`cvar gpu_anatomy_capture N`). Passes are split the way anatomy_passes.py does
(a copy ends a pass; a change of surface / colour 0 / depth starts one). --list prints every pass (index, draws,
surface); default = the biggest pass.
"""
import argparse
import csv
from collections import Counter, defaultdict


def split_passes(rows):
    passes, cur, key = [], [], None
    for r in rows:
        if r['kind'] != 'draw':
            if cur:
                passes.append(cur); cur = []; key = None
            continue
        k = (r['rb_surface_info'], r['rb_color_info0'], r['rb_depth_info'])
        if key is not None and k != key and cur:
            passes.append(cur); cur = []
        key = k
        cur.append(r)
    if cur:
        passes.append(cur)
    return passes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('csv'); ap.add_argument('--pass', dest='pass_i', type=int)
    ap.add_argument('--biggest', action='store_true'); ap.add_argument('--list', action='store_true')
    ap.add_argument('--top', type=int, default=14)
    a = ap.parse_args()
    with open(a.csv, newline='') as f:
        rows = list(csv.DictReader(f))
    passes = split_passes(rows)
    if not passes:
        raise SystemExit(f'pass_shaders: no draws in {a.csv}')
    if a.list:
        for i, p in enumerate(passes):
            print(f'{i:4d}  draws {len(p):5d}  surface {p[0]["rb_surface_info"]} colour0 {p[0]["rb_color_info0"]}')
        return
    big = passes[a.pass_i] if a.pass_i is not None else max(passes, key=len)
    print(f'{a.csv}: {len(rows)} rows, {len(passes)} passes; this pass {len(big)} draws, surface {big[0]["rb_surface_info"]}')
    pairs = Counter((r['vs_hash'], r['ps_hash']) for r in big)
    counts, cp = defaultdict(Counter), defaultdict(float)
    for r in big:
        counts[(r['vs_hash'], r['ps_hash'])][(r['prim'], r['count'])] += 1
        cp[(r['vs_hash'], r['ps_hash'])] += float(r['cp_us'] or 0)
    runs, prev, n = Counter(), None, 0
    for r in big:
        k = (r['vs_hash'], r['ps_hash'], r['count'], r['rb_depthcontrol'], r['rb_colorcontrol'])
        if k == prev:
            n += 1
        else:
            if prev: runs[n] += 1
            prev, n = k, 1
    runs[n] += 1
    print(f'cp_us in pass: {sum(cp.values()):.0f}')
    print(' draws   cp_us  vs               ps               top (prim,count) x n')
    for (vs, ps), c in pairs.most_common(a.top):
        top = ', '.join(f'{p}/{cnt} x{m}' for (p, cnt), m in counts[(vs, ps)].most_common(3))
        print(f'{c:6d} {cp[(vs, ps)]:7.0f}  {vs:16s} {ps:16s} {top}')
    print('runs of >= 8 identical-state draws (length x how many):',
          sorted(((k, m) for k, m in runs.items() if k >= 8), reverse=True)[:12])
    print('draws in such runs:', sum(k * m for k, m in runs.items() if k >= 8))


if __name__ == '__main__':
    main()
