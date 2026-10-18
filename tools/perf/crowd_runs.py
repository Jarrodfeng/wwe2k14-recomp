"""Session 20 item 4: runs of crowd draws in an anatomy CSV (frame 0): run lengths, what ends a run, draw shapes.

    py tools\\perf\\crowd_runs.py ANATOMY_CSV   (crowd = D5E2FCC9 + C1028A64, the pan's crowd shaders)
"""
import csv, collections, sys
rows = [r for r in csv.DictReader(open(sys.argv[1])) if r['frame'] == '0']
crowd = {'D5E2FCC9F76BC40C', 'C1028A64EFCF8229'}
runs = []; cur = []
prev = None
for r in rows:
    if r['kind'] == 'draw' and r['vs_hash'] in crowd:
        if cur and prev is not None and prev['kind'] == 'draw' and prev['vs_hash'] == r['vs_hash'] and prev['state_hash'] == r['state_hash'] and prev['fetch_hash'] == r['fetch_hash']:
            cur.append(r)
        else:
            if cur: runs.append(cur)
            cur = [r]
    else:
        if cur: runs.append(cur); cur = []
    prev = r
if cur: runs.append(cur)
lens = sorted(len(x) for x in runs)
n = sum(lens)
print('crowd draws', n, 'runs (same vs, state_hash, fetch_hash, back to back)', len(runs))
print('run length p50', lens[len(lens)//2], 'max', lens[-1], 'draws in runs >= 8:', sum(l for l in lens if l >= 8), 'in runs >= 16:', sum(l for l in lens if l >= 16))
print('distinct fetch_hash over crowd draws', len({r['fetch_hash'] for r in rows if r['kind']=='draw' and r['vs_hash'] in crowd}),
      'distinct state_hash', len({r['state_hash'] for r in rows if r['kind']=='draw' and r['vs_hash'] in crowd}))
c = collections.Counter((r['vs_consts'], r['ps_consts'], r['count'], r['prim'], r['indexed'], r['vs_tex'], r['ps_tex']) for r in rows if r['kind']=='draw' and r['vs_hash'] in crowd)
print('(vs_consts, ps_consts, count, prim, indexed, vs_tex, ps_tex) top:', c.most_common(6))
# what breaks runs: the row right after a run ends
brk = collections.Counter()
for i, r in enumerate(rows):
    if i and rows[i-1]['kind']=='draw' and rows[i-1]['vs_hash'] in crowd and not (r['kind']=='draw' and r['vs_hash']==rows[i-1]['vs_hash'] and r['state_hash']==rows[i-1]['state_hash'] and r['fetch_hash']==rows[i-1]['fetch_hash']):
        if r['kind'] != 'draw': brk['copy/resolve'] += 1
        elif r['vs_hash'] not in crowd: brk['other shader'] += 1
        elif r['vs_hash'] != rows[i-1]['vs_hash']: brk['other crowd shader'] += 1
        elif r['fetch_hash'] != rows[i-1]['fetch_hash']: brk['fetch constants (vertex buffer / texture)'] += 1
        else: brk['other state (registers)'] += 1
print('what ends a run:', dict(brk))
up = sum(1 for r in rows if r['kind']=='draw' and r['vs_hash'] in crowd and int(r['up_calls'] or 0))
print('crowd draws with an upload before them', up)

