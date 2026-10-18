"""Session 21: in-run A/B of GPU ms per frame, per scene, from bench_report's per-CP-frame CSV.

    py tools/bench/bench_report.py report RUN --ab CVAR      (writes out\\bench\\reports\\cpframes_RUN.csv)
    py tools/perf/ab_gpu.py RUN [--cvar NAME] [--settle 6] [--metric gpu_ms] [--pan 380-500]

The CSV's cvar column (last column 'cvar:NAME') holds the setting per CP frame. Each run of frames with one value is a
segment; the first --settle frames of a segment are dropped. Within a scene, each segment is paired with the one
before it when their values differ (A = --a-value, default 0; B = any other value); the difference B - A per pair gives the mean, a 95 %
interval (normal approximation) and the sign count. Scenes: brock (scene tron_343), bryan (tron_324), pan (Bryan's
wide pan: tron_324 start + --pan guest frames), match. Numbers only; reads our own run output.
"""
import argparse
import csv
import math
import os
import statistics

REPORTS = r'C:\Users\Jarrod\wwe2k14-recomp\out\bench\reports'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--cvar')
    ap.add_argument('--settle', type=int, default=6)
    ap.add_argument('--metric', default='gpu_ms')
    ap.add_argument('--pan', default='380-500')
    ap.add_argument('--a-value', type=float, default=0.0, help='the setting value of arm A (default 0)')
    a = ap.parse_args()
    path = os.path.join(REPORTS, 'cpframes_%s.csv' % a.run)
    rows = list(csv.DictReader(open(path, newline='')))
    col = next((c for c in rows[0] if c.startswith('cvar:') and (not a.cvar or c == 'cvar:' + a.cvar)), None)
    if not col:
        raise SystemExit('no cvar column in ' + path)
    start_324 = min((int(r['guest_frame']) for r in rows if r['scene'] == 'tron_324'), default=None)
    p0, p1 = map(int, a.pan.split('-'))

    def scenes(r):
        out = []
        s = r['scene']
        if s == 'tron_343':
            out.append('brock')
        elif s == 'tron_324':
            out.append('bryan')
            if start_324 is not None and p0 <= int(r['guest_frame']) - start_324 < p1:
                out.append('pan')
        elif s == 'match':
            out.append('match')
        return out

    # Segments: consecutive CP frames with the same cvar value (and the same scene set).
    segs = []
    cur = None
    for r in rows:
        v = r[col].strip()
        if v == '':
            cur = None
            continue
        try:
            m = float(r[a.metric])
        except ValueError:
            continue
        val = 0 if float(v) == a.a_value else 1
        key = (val, tuple(scenes(r)))
        if cur is None or cur['key'] != key:
            cur = dict(key=key, vals=[])
            segs.append(cur)
        cur['vals'].append(m)
    print('run %s, %s by %s (settle %d frames)' % (a.run, a.metric, col[5:], a.settle))
    print('  %-6s %7s %7s | %5s  %8s  %18s  %s' % ('scene', 'A', 'B', 'pairs', 'B-A', '95% interval', 'B lower'))
    for scene in ('brock', 'bryan', 'pan', 'match'):
        ss = [dict(val=s['key'][0], mean=statistics.mean(s['vals'][a.settle:]))
              for s in segs if scene in s['key'][1] and len(s['vals']) > a.settle]
        diffs = []
        for prev, nxt in zip(ss, ss[1:]):
            if prev['val'] != nxt['val']:
                b, aa = (nxt, prev) if nxt['val'] else (prev, nxt)
                diffs.append(b['mean'] - aa['mean'])
        A = [s['mean'] for s in ss if not s['val']]
        B = [s['mean'] for s in ss if s['val']]
        if len(diffs) < 2:
            print('  %-6s  (too few segments: %d pairs)' % (scene, len(diffs)))
            continue
        m = statistics.mean(diffs)
        ci = 1.96 * statistics.stdev(diffs) / math.sqrt(len(diffs))
        print('  %-6s %7.3f %7.3f | %5d  %+8.3f  [%+.3f, %+.3f]  %d/%d' % (
            scene, statistics.mean(A), statistics.mean(B), len(diffs), m, m - ci, m + ci,
            sum(d < 0 for d in diffs), len(diffs)))


if __name__ == '__main__':
    main()
