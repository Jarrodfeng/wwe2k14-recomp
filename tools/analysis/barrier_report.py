"""Frame-barrier report (Session 8 task B): which barrier client held the render thread in each slow frame.

Usage: py tools/analysis/barrier_report.py RUN [--ms 25] [--logs DIR] [--top 40] [--from-frame 60]

Reads logs/barrier_NNN.csv (--wwe2k14_barrier_probe=true, src/barrier_probe.h) and logs/frames_NNN.csv.
Per client slot (named by the guest function that contains its barrier call's return address, via tools/q, plus its
db name): median / p99 / max of its wake latency (release call -> its release wait returned) and of its work
(wake -> arrival), over all rows. Then for every frame of at least --ms (frames_NNN.csv; loading frames over 400 ms
left out): the render thread's own time to the wait (wait_begin), its wait, and the client that arrived last, with
that client's wake latency and work. The barrier row of frame F is the row whose `frame` column is F-1 (the row is
written before that iteration's present, the VdSwap that ends frame F). Pairing check (on the slow rows only, since
almost every frame is 16.7 ms and any offset matches a normal frame): for the rows where the render thread waited
>= 10 ms for its clients, frames_NNN.csv's frame_ms of frame = row + k for k = -1..+3, and how many of those rows
have frame_ms >= 0.9 x that wait (the frame that contains the wait cannot be much shorter than it); the best k is
printed and must be +1 (runs 327/333/334/341: 13 of 13 at +1, at most 7 of 13 at any other k; the neighbours of a
slow frame are often slow too). The render thread's own time (release -> wait_begin) and its wait are given
as percentiles over the whole run and per scene (scene windows: the anchor file opens as in tools/bench/fps60.py, but
each scene runs to the next anchor; fps60.py ends the second entrance 80 frames before M3.PAC and starts the match
120 frames after it, so per-scene counts here and in fps60.py differ slightly).
Everything here is our own run output; nothing from the game is read.
"""
import argparse
import csv
import json
import os
import re
import statistics
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
LOGS = os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs')


def db_names():
    names = {}
    ddir = os.path.join(ROOT, 'db')
    for fn in os.listdir(ddir):
        if not fn.endswith('.json'):
            continue
        try:
            d = json.load(open(os.path.join(ddir, fn), encoding='utf-8'))
        except Exception:
            continue
        ents = d if isinstance(d, list) else d.get('functions', d.get('entries', []))
        for e in ents if isinstance(ents, list) else []:
            if isinstance(e, dict) and 'address' in e and 'name' in e:
                names['sub_' + e['address'].upper().replace('0X', '')] = e['name']
    return names


def func_of(addr):
    try:
        out = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'q', 'q.py'), 'addr', f'0x{addr:08X}'],
                             capture_output=True, text=True, timeout=60).stdout
        m = re.search(r'function:\s*(\S+)', out)
        return m.group(1) if m else '?'
    except Exception:
        return '?'


def pct(v, p):
    if not v:
        return float('nan')
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]


def load(logs, n):
    rows = []
    path = os.path.join(logs, f'barrier_{n}.csv')
    if not os.path.exists(path):
        sys.exit(f'barrier_report: {path} not found (run {n} needs --wwe2k14_barrier_probe=true)')
    with open(path, newline='') as f:
        if next(f, None) is None:
            sys.exit(f'barrier_report: {path} is empty')
        for line in f:
            p = line.strip().split(',')
            if len(p) < 5:
                continue
            try:
                r = {'frame': int(p[0]), 't': float(p[1]), 'wb': float(p[2]), 'we': float(p[3]), 'slots': []}
                for s in p[5:]:
                    lr, tid, wk, ar = s.split(';')
                    r['slots'].append((int(lr, 16), int(tid), float(wk), float(ar)))
            except ValueError:
                continue  # a truncated last line
            rows.append(r)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--ms', type=float, default=25.0)
    ap.add_argument('--logs', default=LOGS)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--from-frame', type=int, default=60)
    a = ap.parse_args()
    n = re.sub(r'\D', '', os.path.basename(a.run)).zfill(3)
    rows = load(a.logs, n)
    if not rows:
        sys.exit(f'barrier_report: barrier_{n}.csv has no complete rows')
    frames = {}
    fpath = os.path.join(a.logs, f'frames_{n}.csv')
    if not os.path.exists(fpath):
        sys.exit(f'barrier_report: {fpath} not found')
    with open(fpath, newline='') as f:
        for r in csv.DictReader(f):
            frames[int(r['frame'])] = float(r['frame_ms'])
    by_frame = {r['frame']: r for r in rows}
    dbn = db_names()
    # slot identity: (index, lr of its first row, tid)
    labels = {}
    for r in rows:
        for i, (lr, tid, wk, ar) in enumerate(r['slots']):
            if i not in labels:
                fn = func_of(lr)
                labels[i] = f'{fn}' + (f' [{dbn[fn]}]' if fn in dbn else '') + f' tid {tid}'
    print(f'# barrier report, run {n}: {len(rows)} rows (frames {rows[0]["frame"]}..{rows[-1]["frame"]}), '
          f'{len(labels)} client slots')
    # Pairing check on the slow rows (a check over all rows cannot tell offsets apart: almost every frame is 16.7 ms).
    slow_rows = [r for r in rows if r['we'] - r['wb'] >= 10.0 and r['frame'] >= a.from_frame]
    if slow_rows:
        res = []
        for k in range(-1, 4):
            v = [frames[r['frame'] + k] for r in slow_rows if r['frame'] + k in frames]
            hit = sum(1 for r in slow_rows if frames.get(r['frame'] + k, 0.0) >= 0.9 * (r['we'] - r['wb']))
            res.append((hit, k, statistics.median(v) if v else float('nan')))
        top = max(h for h, _, _ in res)
        best = [k for h, k, _ in res if h == top]
        verdict = (f' -> best k={best[0]:+d}' if len(best) == 1 else
                   ' -> best k ambiguous (' + ' '.join(f'{k:+d}' for k in best) + ')')
        if best != [1]:
            verdict += '  WARNING: expected +1 alone; the per-frame pairing below may be off'
        print(f'# pairing check over the {len(slow_rows)} rows with a client wait >= 10 ms (frame_ms of frame = row + k '
              f'>= 0.9 x wait): ' + ', '.join(f'k={k:+d} {h}/{len(slow_rows)} (median {m:.1f} ms)'
                                                     for h, k, m in res) + verdict)
    else:
        print('# pairing check: no row with a client wait >= 10 ms (nothing to check it on)')
    print('# per slot over all rows (ms after the release call): wake p50/p99/max | work (wake->arrive) p50/p99/max '
          '| arrived last (render had to wait > 0.5 ms for it)')
    last_count = {}
    for r in rows:
        if r['frame'] < a.from_frame:
            continue
        if r['we'] - r['wb'] > 0.5 and r['slots']:
            i = max(range(len(r['slots'])), key=lambda k: r['slots'][k][3])
            last_count[i] = last_count.get(i, 0) + 1
    for i, lab in labels.items():
        wk = [r['slots'][i][2] for r in rows if len(r['slots']) > i and r['slots'][i][2] >= 0 and r['frame'] >= a.from_frame]
        wo = [r['slots'][i][3] - r['slots'][i][2] for r in rows
              if len(r['slots']) > i and r['slots'][i][3] >= 0 and r['frame'] >= a.from_frame]
        print(f'  slot {i}: {lab}\n      wake {pct(wk, 50):6.3f}/{pct(wk, 99):6.3f}/{max(wk) if wk else float("nan"):7.3f}'
              f' | work {pct(wo, 50):6.3f}/{pct(wo, 99):6.3f}/{max(wo) if wo else float("nan"):7.3f}'
              f' | last {last_count.get(i, 0)} rows')
    # Per scene (anchors as in tools/bench/fps60.py): frames > 20 / > --ms, split by where the time went.
    anch = {}
    with open(os.path.join(a.logs, f'wwe2k14_{n}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(r"\[file\] \S+ '([^']+)'.*frame=(\d+)", line)
            if m:
                p = m.group(1).lower()
                for k in ('343.bik2', '324.bik2', '103.bik2', 'm3.pac'):
                    if k in p and k not in anch:
                        anch[k] = int(m.group(2))
    order = sorted(anch.items(), key=lambda kv: kv[1])

    def scene_of(fr):
        s = 'pre'
        for k, v in order:
            if v <= fr:
                s = k
        return s

    # The render thread's own time to the wait (wb) and its wait (we - wb), whole run and per scene (row of frame F
    # = row frame F-1, so the scene is that of frame+1).
    groups = {'whole run': []}
    for r in rows:
        if r['frame'] < a.from_frame:
            continue
        groups['whole run'].append(r)
        groups.setdefault(scene_of(r['frame'] + 1), []).append(r)
    print(f'# render thread per row (frames >= {a.from_frame}): own time to the wait p50 / p99 / p99.9 | wait for the '
          f'clients p50 / p99 / p99.9 (ms); scene = anchor file open .. next anchor')
    for g in ['whole run'] + [k for k, _ in order] + (['pre'] if 'pre' in groups else []):
        rs = groups.get(g)
        if not rs:
            continue
        own = [r['wb'] for r in rs]
        wt = [r['we'] - r['wb'] for r in rs]
        print(f'  {g:9s} rows {len(rs):6d}: own {pct(own, 50):5.2f} / {pct(own, 99):5.2f} / {pct(own, 99.9):6.2f} | '
              f'wait {pct(wt, 50):5.2f} / {pct(wt, 99):5.2f} / {pct(wt, 99.9):6.2f}')

    print(f'# per scene: frames > 20 ms | >= {a.ms} ms, each split: client-bound (render waited >= 10 ms for the '
          f'clients after its own work; normal frames at the 4K preset: p99.9 7 ms, run 327; last arriver named) / render-side (the render thread\'s own time: command list, GPU '
          f'fence, present)')
    tally = {}
    for fr, ms in frames.items():
        if ms <= 20 or ms >= 400 or fr < a.from_frame:
            continue
        r = by_frame.get(fr - 1)
        sc = scene_of(fr)
        t = tally.setdefault(sc, {'gt20': 0, 'gtms': 0, 'client20': 0, 'clientms': 0, 'last': {}})
        t['gt20'] += 1
        big = ms >= a.ms
        t['gtms'] += big
        if r and r['slots'] and (r['we'] - r['wb']) >= 10.0:
            t['client20'] += 1
            t['clientms'] += big
            i = max(range(len(r['slots'])), key=lambda k: r['slots'][k][3])
            t['last'][i] = t['last'].get(i, 0) + 1
    for sc, t in sorted(tally.items(), key=lambda kv: anch.get(kv[0], -1)):
        last = ', '.join(f'slot {i} x{c}' for i, c in sorted(t['last'].items()))
        print(f'  {sc:9s} > 20 ms: {t["gt20"]:3d} (client-bound {t["client20"]}, render-side '
              f'{t["gt20"] - t["client20"]}) | >= {a.ms:.0f} ms: {t["gtms"]:3d} (client-bound {t["clientms"]}, '
              f'render-side {t["gtms"] - t["clientms"]}) | last arriver: {last or "-"}')

    slow = sorted(f for f, ms in frames.items() if a.ms <= ms < 400 and f >= a.from_frame)
    print(f'\n# frames >= {a.ms} ms (< 400): {len(slow)}; per frame: frame ms | render: own work to the wait '
          f'(wait_begin), wait | last client: wake, work, arrive (ms after release) | others\' work')
    shown = 0
    for fr in slow:
        r = by_frame.get(fr - 1)
        if r is None:
            print(f'  frame {fr:6d} {frames[fr]:6.1f} ms  (no barrier row)')
            continue
        s = r['slots']
        if not s:
            continue
        i = max(range(len(s)), key=lambda k: s[k][3])
        lr, tid, wk, ar = s[i]
        others = ' '.join(f'{k}:{s[k][3] - s[k][2]:.1f}' for k in range(len(s)) if k != i and s[k][3] >= 0)
        print(f'  frame {fr:6d} {frames[fr]:6.1f} ms | render own {r["wb"]:6.2f}, wait {r["we"] - r["wb"]:6.2f} | '
              f'last slot {i} wake {wk:6.2f} work {ar - wk:6.2f} arrive {ar:6.2f} | others {others}')
        shown += 1
        if shown >= a.top:
            break


if __name__ == '__main__':
    main()
