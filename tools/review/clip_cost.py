"""Frame-time cost of in-game clip capture (src/clip_capture.h), from a run's frames_NNN.csv and log.

    py tools\\review\\clip_cost.py RUN                 in-run A/B: each in-game clip's frames vs the same number of
                                                     frames just before it (tools/review/routes/cliptest_ab_menu.txt
                                                     alternates 5 s off / 5 s on, so "before" is a clean baseline)
    py tools\\review\\clip_cost.py RUN --review NAME [--vs RUN2 ...]
                                                     the route's "review: clip_start/clip_stop NAME" window (review
                                                     routes), for this run and others (cross-run: e.g. a desktop-
                                                     capture run of the same route and arm as the baseline; cross-
                                                     run numbers vary ~20% in heavy scenes)

Per window: frames, fps, mean / p50 / p95 / p99 / max frame ms, frames over 20 ms and over 1.25 x the median.
Also prints each clip's own counts from the game's "[wwe2k14][clip] done" line. Reads only numbers (no imagery).
"""
import argparse
import csv
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
LOGS = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'logs')


def frame_ms(run):
    with open(os.path.join(LOGS, f'frames_{int(run):03d}.csv'), encoding='utf-8') as f:
        return {int(r['frame']): float(r['frame_ms']) for r in csv.DictReader(f)}


def log_lines(run):
    with open(os.path.join(LOGS, f'wwe2k14_{int(run):03d}.log'), encoding='utf-8', errors='replace') as f:
        return f.readlines()


def stats(ms):
    if not ms:
        return None
    s = sorted(ms)
    q = lambda p: s[min(len(s) - 1, int(p * len(s)))]  # noqa: E731
    med = q(0.5)
    return {'frames': len(ms), 'fps': round(len(ms) / (sum(ms) / 1000), 2), 'mean': round(sum(ms) / len(ms), 2),
            'p50': round(med, 2), 'p95': round(q(0.95), 2), 'p99': round(q(0.99), 2), 'max': round(s[-1], 1),
            'gt20': sum(m > 20 for m in ms), 'gt125med': sum(m > 1.25 * med for m in ms)}


def window(fm, f0, f1):
    return [fm[f] for f in range(f0 + 1, f1 + 1) if f in fm]


def fmt(st):
    if not st:
        return 'no frames'
    return (f"{st['frames']:5d} fr {st['fps']:6.2f} fps  mean {st['mean']:6.2f}  p50 {st['p50']:6.2f}  "
            f"p95 {st['p95']:6.2f}  p99 {st['p99']:6.2f}  max {st['max']:6.1f}  >20ms {st['gt20']:3d}  "
            f">1.25xmed {st['gt125med']:3d}")


def ab(run):
    fm = frame_ms(run)
    lines = log_lines(run)
    starts, stops, done = {}, {}, {}
    for l in lines:
        m = re.search(r'\[clip\] clip_start (\S+) at .*\(frame (\d+)\)', l)
        if m:
            starts[m.group(1)] = int(m.group(2))
        m = re.search(r'\[clip\] clip_stop (\S+) \(frame (\d+);', l)
        if m:
            stops[m.group(1)] = int(m.group(2))
        m = re.search(r'\[clip\] done (\S+): (.*?) \(frames', l)
        if m:
            done[m.group(1)] = m.group(2)
    print(f'run {run}: {len(starts)} clips')
    on_all, off_all = [], []
    for name, f0 in starts.items():
        f1 = stops.get(name)
        if f1 is None:
            continue
        n = f1 - f0
        on, off = window(fm, f0, f1), window(fm, f0 - n, f0)
        on_all += on
        off_all += off
        print(f'  {name} frames {f0}..{f1}')
        print(f'    before (no clip): {fmt(stats(off))}')
        print(f'    clip running    : {fmt(stats(on))}')
        if name in done:
            print(f'    clip: {done[name][:300]}')
    if on_all:
        print('  all clips:')
        print(f'    before (no clip): {fmt(stats(off_all))}')
        print(f'    clip running    : {fmt(stats(on_all))}')


def review(runs, name):
    for run in runs:
        fm = frame_ms(run)
        f0 = f1 = None
        for l in log_lines(run):
            m = re.search(r'review: clip_(start|stop) (\S+) \(frame (\d+)\)', l)
            if m and m.group(2) == name:
                if m.group(1) == 'start':
                    f0 = int(m.group(3))
                else:
                    f1 = int(m.group(3))
        clip = ''
        if any('[clip] done ' + name + ':' in l for l in log_lines(run)):
            clip = ' (in-game clip running)'
        if f0 is None or f1 is None:
            print(f'run {run}: no review window {name}')
            continue
        print(f'run {run} {name} {f0}..{f1}{clip}: {fmt(stats(window(fm, f0, f1)))}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run', type=int)
    ap.add_argument('--review', help='review window name (e2, match)')
    ap.add_argument('--vs', type=int, nargs='*', default=[], help='more runs for --review')
    a = ap.parse_args()
    if a.review:
        review([a.run] + a.vs, a.review)
    else:
        ab(a.run)


if __name__ == '__main__':
    sys.exit(main())
