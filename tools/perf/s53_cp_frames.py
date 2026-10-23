"""Session 53 item 2.7: per command-processor frame (between two `cpf` timeline records) its wall time, draws, texture
loads (count, KiB written into images), uploads (KiB) - the longest ones, or the ones in a time window.

    py tools\\perf\\s53_cp_frames.py RUN [--top 15] [--from S --to S] [--logs DIR]

Times are seconds since the first timeline record.
"""
import argparse
import csv
import os

ap = argparse.ArgumentParser()
ap.add_argument('run')
ap.add_argument('--top', type=int, default=15)
ap.add_argument('--from', dest='t0', type=float)
ap.add_argument('--to', dest='t1', type=float)
ap.add_argument('--logs', default=r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs')
a = ap.parse_args()
rows = list(csv.DictReader(open(os.path.join(a.logs, f'timeline_{a.run}.csv'))))
base = int(rows[0]['t_ns'])
frames, cur, last_t = [], {}, None
for r in rows:
    e = r['event']
    if not e.startswith('cpf'):
        continue
    t = (int(r['t_ns']) - base) / 1e9
    cur[e] = tuple(int(r[k] or '0', 0) for k in ('a', 'b', 'c'))  # decimal or 0x hex
    if e == 'cpf':
        if last_t is not None:
            frames.append((t, (t - last_t) * 1000, dict(cur)))
        last_t, cur = t, {}
if a.t0 is not None:
    sel = [f for f in frames if a.t0 <= f[0] <= a.t1]
else:
    sel = sorted(frames, key=lambda f: -f[1])[:a.top]
    sel.sort()
print(f'run {a.run}: {len(frames)} CP frames')
print('     t(s)    ms  draws  texloads  tex KiB  upload KiB  pipeline binds changed')
for t, ms, c in sel:
    d = c.get('cpf', (0, 0, 0))[1]
    tl = c.get('cpf_texload', (0, 0, 0))
    up = c.get('cpf_rr', (0, 0, 0))[2]
    fc = c.get('cpf_fc', (0, 0, 0))[2]
    print(f'  {t:8.2f} {ms:6.1f} {d:6d} {tl[0]:8d} {tl[2]:9d} {up:10d} {fc:8d}')
