"""In-run A/B analysis with optional difference-in-differences against an A/A run (Session 6).

    py tools/bench/ab_paired.py RUN [RUN ...] [--baseline AA_RUN] [--settle 6] [--logs DIR]

For runs made with tools/input_scripts/make_ab.py (one setting toggled every PERIOD guest frames; the timeline
'cvar' events mark each flip, any non-zero value = arm B). Independent of bench_report.py (cross-check).
- CP frame = cp_swap to cp_swap on the command-processor thread; busy = span - (cp_idle + wrm + hwait + intr), the
  definition of tools/analysis/timeline.py. Frames within --settle CP frames of a flip are dropped.
- Scenes from the game log's [file] lines (frame=N): tron_343 (Brock), tron_103 heavy (+0..+700) / rest, or
  tron_324 parts, and the match (M3.PAC +300..+3900).
- Per scene: arm means, and the mean of adjacent A/B segment-pair differences (B - A) with a 95% CI.
- --baseline AA_RUN: an A/A run with the same script schedule (a no-op setting toggled). Its pair differences are
  a content bias of the schedule (e.g. a heavy shot always landing in a B segment). With the baseline, each
  pair's difference has the baseline's matching pair difference (same scene, same pair index) subtracted.
Numbers only; reads our own run output.
"""
import argparse
import bisect
import csv
import math
import os
import re
import statistics
from collections import defaultdict

LOGS = r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs'


def load_run(run, logs, settle):
    run = str(run).zfill(3)
    rows = []
    with open(os.path.join(logs, f'timeline_{run}.csv'), newline='') as f:
        for r in csv.DictReader(f):
            try:
                rows.append((int(r['t_ns']), int(r['thread']), r['event'], int(r['a'], 16), int(r['b'], 16),
                             int(r['c'], 16)))
            except (ValueError, KeyError, TypeError):
                break
    rows.sort()
    threads = defaultdict(set)
    for t, th, ev, *_ in rows:
        threads[ev].add(th)
    cp = next(iter(threads['cp_swap']))
    swaps = [t for t, th, ev, *_ in rows if ev == 'cp_swap' and th == cp]
    vd = [(t, a) for t, th, ev, a, b, c in rows if ev == 'vdswap']
    vd_t = [x[0] for x in vd]

    def covered_fn(name, thread):
        ivs, st = [], None
        for t, th, ev, *_ in rows:
            if th != thread:
                continue
            if ev == name + '_b':
                st = t
            elif ev == name + '_e' and st is not None:
                ivs.append((st, t))
                st = None
        starts = [s for s, _ in ivs]
        cum = [0]
        for s, e in ivs:
            cum.append(cum[-1] + e - s)

        def cov(lo, hi):
            tot = 0
            i = bisect.bisect_left(starts, lo)
            if i > 0 and ivs[i - 1][1] > lo:
                tot += min(ivs[i - 1][1], hi) - lo
            j = bisect.bisect_left(starts, hi)
            if j > i:
                tot += cum[j] - cum[i]
                if ivs[j - 1][1] > hi:
                    tot -= ivs[j - 1][1] - hi
            return tot
        return cov

    cov = {n: covered_fn(n, cp) for n in ('cp_idle', 'wrm', 'hwait', 'intr', 'drain')}
    sub = next(iter(threads.get('asub_b', {None})))
    asub = covered_fn('asub', sub) if sub is not None else (lambda lo, hi: 0)
    cv = [(t, b) for t, th, ev, a, b, c in rows if ev == 'cvar']
    cv_t = [x[0] for x in cv]
    frames, since, last = [], 0, None
    for k in range(len(swaps) - 1):
        lo, hi = swaps[k], swaps[k + 1]
        busy = (hi - lo) - sum(cov[n](lo, hi) for n in ('cp_idle', 'wrm', 'hwait', 'intr'))
        i = bisect.bisect_right(vd_t, lo) - 1
        j = bisect.bisect_right(cv_t, lo) - 1
        arm = (1 if cv[j][1] else 0) if j >= 0 else None
        since = 0 if arm != last else since + 1
        last = arm
        frames.append(dict(gf=vd[i][1] if i >= 0 else -1, span=(hi - lo) / 1e6, busy=busy / 1e6,
                           asub=asub(lo, hi) / 1e6, drain=cov['drain'](lo, hi) / 1e6, arm=arm, ok=since >= settle))
    anchors = []
    for line in open(os.path.join(logs, f'wwe2k14_{run}.log'), encoding='utf-8', errors='replace'):
        m = re.search(r"\[file\] \S+ '([^']+)'.*frame=(\d+)", line)
        if m:
            anchors.append((int(m.group(2)), m.group(1).lower()))

    def first(pat, after=0):
        return next((f for f, p in anchors if f >= after and pat in p), None)
    scenes = {}
    t343 = first('343.bik2')
    t103, t324 = first('103.bik2'), first('324.bik2')
    second = t103 if t103 is not None else t324
    if t343 is not None and second is not None:
        scenes['tron_343'] = (t343, second)
        end = first('loadhd.pac', second) or second + 3700
        if t103 is not None:
            scenes['tron_103 heavy'] = (second, second + 700)
            scenes['tron_103 rest'] = (second + 700, end)
        else:
            scenes['tron_324 early'] = (second, second + 380)
            scenes['tron_324 pan'] = (second + 380, second + 500)
            scenes['tron_324 rest'] = (second + 500, end)
    m3 = first('m3.pac', second or 0)
    if m3 is not None:
        scenes['match'] = (m3 + 300, m3 + 3900)
    out = {}
    for name, (a, b) in scenes.items():
        sel = [f for f in frames if a <= f['gf'] < b and f['ok'] and f['arm'] in (0, 1)]
        segs = []
        for f in sel:
            if segs and segs[-1]['arm'] == f['arm'] and f['gf'] - segs[-1]['last'] <= 3:
                segs[-1]['f'].append(f)
                segs[-1]['last'] = f['gf']
            else:
                segs.append(dict(arm=f['arm'], f=[f], last=f['gf']))
        diffs = []
        for i in range(len(segs) - 1):
            if segs[i]['arm'] != segs[i + 1]['arm']:
                sa = segs[i] if segs[i]['arm'] == 0 else segs[i + 1]
                sb = segs[i + 1] if segs[i]['arm'] == 0 else segs[i]
                diffs.append(statistics.mean(x['busy'] for x in sb['f']) - statistics.mean(x['busy'] for x in sa['f']))
        arm = {k: [f for f in sel if f['arm'] == k] for k in (0, 1)}
        out[name] = dict(diffs=diffs, arm=arm)
    return out


def ci(xs):
    if len(xs) < 2:
        return float('nan'), float('nan')
    return statistics.mean(xs), 1.96 * statistics.stdev(xs) / math.sqrt(len(xs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--baseline')
    ap.add_argument('--settle', type=int, default=6)
    ap.add_argument('--logs', default=LOGS)
    args = ap.parse_args()
    base = load_run(args.baseline, args.logs, args.settle) if args.baseline else None
    for run in args.runs:
        res = load_run(run, args.logs, args.settle)
        print(f'run {run}' + (f' (minus A/A run {args.baseline})' if base else ''))
        for name, r in res.items():
            A, B = r['arm'][0], r['arm'][1]
            if not A or not B:
                continue
            mA, mB = statistics.mean(f['busy'] for f in A), statistics.mean(f['busy'] for f in B)
            d, w = ci(r['diffs'])
            faster = sum(x < 0 for x in r['diffs'])
            line = (f'  {name:15s} A {mA:6.2f}  B {mB:6.2f} ms | pairs {len(r["diffs"]):3d}: {d:+6.2f} +-{w:4.2f} '
                    f'({faster}/{len(r["diffs"])} faster)')
            if base and name in base:
                bd = base[name]['diffs']
                n = min(len(bd), len(r['diffs']))
                dd = [r['diffs'][i] - bd[i] for i in range(n)]
                d2, w2 = ci(dd)
                line += f' | DiD {d2:+6.2f} +-{w2:4.2f} ({sum(x < 0 for x in dd)}/{n} faster)'
            line += (f' | slow>=45 A {sum(f["span"] >= 45 for f in A)} B {sum(f["span"] >= 45 for f in B)}'
                     f' | asub A {statistics.mean(f["asub"] for f in A):.2f} B {statistics.mean(f["asub"] for f in B):.2f}')
            print(line)


if __name__ == '__main__':
    main()
