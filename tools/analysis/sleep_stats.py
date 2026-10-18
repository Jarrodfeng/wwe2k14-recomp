"""Totals of the [wwe2k14][sleep] lines of a run (--wwe2k14_sleep_stats=true, src/precise_sleep.h).

Usage: py tools/analysis/sleep_stats.py RUN [RUN...] [--from-frame N] [--to-frame N] [--logs DIR]

Per run and path (precise / sdk-eligible / sdk-other; builds before the Session 8 fixer pass logged "sdk" for both
ReXGlue paths): delays, mean requested and mean measured ms, mean overshoot (measured - requested), and the summed
histogram of the 1-ms requests. Each log line covers the ~5 s before it ("frame N" = the guest frame when it was
written). Lines whose mean requested is over 60,000 ms are skipped and counted: builds s8_tB_2 and earlier also
counted alertable "forever" delays (runs 333/334), which the current code excludes. Our own run output only.
"""
import argparse
import os
import re

LOGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out', 'build', 'win-amd64-release',
                    'logs')
LINE = re.compile(r'\[wwe2k14\]\[sleep\] frame (\d+) (\S+): (\d+) delays, mean requested ([0-9.]+) ms, '
                  r'mean measured ([0-9.]+) ms; 1-ms requests \(ms\): (.*)$')
BIN = re.compile(r'(\S+) (\d+)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--from-frame', type=int, default=0)
    ap.add_argument('--to-frame', type=int, default=1 << 62)
    ap.add_argument('--logs', default=LOGS)
    a = ap.parse_args()
    for run in a.runs:
        n = re.sub(r'\D', '', os.path.basename(run)).zfill(3)
        path = os.path.join(a.logs, f'wwe2k14_{n}.log')
        if not os.path.exists(path):
            print(f'run {n}: {path} not found')
            continue
        tot = {}
        skipped = {}
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = LINE.search(line.rstrip())
                if not m:
                    continue
                fr, p, c = int(m.group(1)), m.group(2).rstrip(':'), int(m.group(3))
                if not (a.from_frame <= fr <= a.to_frame):
                    continue
                rq, ac = float(m.group(4)), float(m.group(5))
                t = tot.setdefault(p, {'c': 0, 'rq': 0.0, 'ac': 0.0, 'bins': {}, 'order': []})
                if rq > 60000.0:
                    # Means unusable; the 1-ms histogram only counts requests of exactly 1 ms, so it is kept.
                    skipped[p] = skipped.get(p, 0) + 1
                else:
                    t['c'] += c
                    t['rq'] += rq * c
                    t['ac'] += ac * c
                for name, v in BIN.findall(m.group(6).replace('|', ' ')):
                    if name not in t['bins']:
                        t['order'].append(name)
                    t['bins'][name] = t['bins'].get(name, 0) + int(v)
        if not tot and not skipped:
            print(f'run {n}: no [wwe2k14][sleep] lines (needs --wwe2k14_sleep_stats=true)')
            continue
        print(f'run {n} (frames {a.from_frame}..{"end" if a.to_frame >= 1 << 62 else a.to_frame}):')
        for p, t in sorted(tot.items()):
            c = t['c']
            hist = ' | '.join(f'{k} {t["bins"][k]}' for k in t['order'])
            means = (f'mean requested {t["rq"] / c:8.3f} ms, measured {t["ac"] / c:8.3f} ms, '
                     f'overshoot {(t["ac"] - t["rq"]) / c:6.3f} ms' if c else 'no usable means')
            print(f'  {p:13s} {c:8d} delays, {means}; 1-ms requests: {hist}')
        for p, k in sorted(skipped.items()):
            print(f'  {p:13s} {k} windows left out of the means (mean requested > 60 s: old-build stats that counted '
                  f'"forever" delays; their 1-ms histograms are kept)')


if __name__ == '__main__':
    main()
