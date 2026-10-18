"""Where in a route do the dropped frames fall? (Session 8 task B, entrance drops.)

Usage: py tools/analysis/drop_positions.py RUN [RUN...] [--ms 25] [--ms30 40] [--logs DIR]

For each run: the scene anchors from the file-open log lines (first open of 343.bik2 / 324.bik2 / 103.bik2 /
m3.pac, as in tools/bench/fps60.py), then every frame at least --ms long (--ms30 inside the 30 fps entrance windows
of a default-settings run, where every frame is ~33 ms) with its wall-clock time since the scene's anchor
(t_seconds of frames_NNN.csv). Same-content runs that drop at the same seconds after the anchor point at work tied
to the content (a scripted cut, a load), not at random host stalls.
Frames over 400 ms (loading screens) are left out. Reads only our own run output (logs/frames_NNN.csv and the log).
"""
import argparse
import csv
import os
import re

LOGS = os.path.join(os.path.dirname(__file__), '..', '..', 'out', 'build', 'win-amd64-release', 'logs')
KEYS = ('343.bik2', '324.bik2', '103.bik2', 'm3.pac')


def anchors(logs, n):
    anch = {}
    rate = []  # (frame, word) from the [pacing] lines
    with open(os.path.join(logs, f'wwe2k14_{n}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(r"\[file\] \S+ '([^']+)'.*frame=(\d+)", line)
            if m:
                p = m.group(1).lower()
                for k in KEYS:
                    if k in p and k not in anch:
                        anch[k] = int(m.group(2))
                continue
            m = re.search(r'\[pacing\] frame (\d+): frame-rate word (\d+) -> (\d+)', line)
            if m:
                rate.append((int(m.group(1)), int(m.group(3))))
    return anch, rate


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--ms', type=float, default=25.0)
    ap.add_argument('--ms30', type=float, default=40.0)
    ap.add_argument('--logs', default=LOGS)
    a = ap.parse_args()
    for run in a.runs:
        n = re.sub(r'\D', '', os.path.basename(run)).zfill(3)
        anch, rate = anchors(a.logs, n)
        rows = []
        with open(os.path.join(a.logs, f'frames_{n}.csv'), newline='') as f:
            for r in csv.DictReader(f):
                rows.append((int(r['frame']), float(r['t_seconds']), float(r['frame_ms'])))
        t_of = {fr: t for fr, t, _ in rows}

        def word_at(fr):
            w = 60
            for f0, w0 in rate:
                if f0 <= fr:
                    w = w0
            return w

        order = sorted(anch.items(), key=lambda kv: kv[1])
        print(f'run {n}: anchors ' + ', '.join(f'{k}@{v}' for k, v in order))
        for fr, t, ms in rows:
            if ms >= 400:
                continue
            lim = a.ms30 if word_at(fr) == 30 else a.ms
            if ms < lim:
                continue
            scene, t0 = '-', None
            for k, v in order:
                if v <= fr:
                    scene, t0 = k, t_of.get(v)
            dt = f'{t - t0:8.2f}s' if t0 is not None else '       -'
            print(f'  frame {fr:6d} {ms:6.1f} ms  {scene:9s} +{dt}  (word {word_at(fr)})')


if __name__ == '__main__':
    main()
