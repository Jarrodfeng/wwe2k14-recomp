r"""The job system's scaling curve (Session 95): per `log arm W=N` window of short\s95_jobs_curve.txt, the frame ms and game-side numbers from
frames_NNN.csv, for one launched uncapped run.
    py -I tools\native\jobs_curve.py RUN [--dir LOGS]
"""
import argparse
import csv
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT = os.environ.get('LOGS') or str(ROOT / 'out' / 'build' / 'win-amd64-release' / 'logs')
ARM = re.compile(r'arm (W=\S+) \(frame (\d+)\)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run')
    ap.add_argument('--dir', default=DEFAULT)
    a = ap.parse_args()
    arms = []
    with open(Path(a.dir) / f'wwe2k14_{int(a.run):03d}.log', encoding='utf-8', errors='replace') as f:
        for line in f:
            m = ARM.search(line)
            if m:
                arms.append((m.group(1), int(m.group(2))))
    rows = list(csv.DictReader(open(Path(a.dir) / f'frames_{int(a.run):03d}.csv', encoding='utf-8', errors='replace')))
    for i, (name, f0) in enumerate(arms):
        f1 = arms[i + 1][1] if i + 1 < len(arms) else f0 + 600
        ms = sorted(float(r['frame_ms']) for r in rows if f0 + 60 <= int(r['frame']) < f1 and r['frame_ms'])
        if not ms:
            continue
        avg = sum(ms) / len(ms)
        print(f'{name:7s} frames {len(ms):4d}  avg {avg:6.3f} ms ({1000 / avg:6.1f} fps)  p50 {ms[len(ms) // 2]:6.3f}  p99 {ms[int(len(ms) * .99)]:6.3f}')


if __name__ == '__main__':
    main()
