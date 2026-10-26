r"""Compile stutter of PC runs (Session 69, the Intel Iris Xe tester): per run and per segment between the route's
marks - frames, time, frames over 50 / 100 ms, seconds lost in frames over 100 ms, the worst frame - and from the log's
[s58][gpu] lines: native pipelines made, their compile time and the draws skipped while compiling.

    py -I tools\native\stutter_report.py RUN [RUN ...]            (logs and frames in out\build\win-amd64-release\logs)
    py -I tools\native\stutter_report.py --dir LOGDIR RUN ...     (a tester's files: wwe2k14_RUN.log + frames_RUN.csv)
"""
import argparse
import csv
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MARK = re.compile(r'\[input\]\S* (?:line \d+: )?(.*?)\r? \(frame (\d+)\)')
S58 = re.compile(r'\[s58\]\[gpu\] frames (\d+)-(\d+): pipelines made (\d+) \(avg ([0-9.]+) ms\) \+ native (\d+) '
                 r'\(avg ([0-9.]+) ms\).*draws skipped while compiling (\d+)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--dir', default=os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs'))
    a = ap.parse_args()
    for run in a.runs:
        log = os.path.join(a.dir, f'wwe2k14_{run}.log')
        frames_path = os.path.join(a.dir, f'frames_{run}.csv')
        marks, native, native_ms, skipped = [], 0, 0.0, 0
        with open(log, encoding='utf-8', errors='replace', newline='\n') as f:
            for line in f:
                m = MARK.search(line)
                if m and not m.group(1).startswith(('waiting for', 'saw ')):
                    marks.append((m.group(1).strip(), int(m.group(2))))
                m = S58.search(line)
                if m:
                    native += int(m.group(5))
                    native_ms += int(m.group(5)) * float(m.group(6))
                    skipped += int(m.group(7))
        rows = []
        with open(frames_path, newline='') as f:
            for r in csv.DictReader(f):
                try:
                    rows.append((int(r['frame']), float(r['frame_ms'])))
                except (ValueError, KeyError, TypeError):
                    break
        print(f'== run {run}: native pipelines {native} ({native_ms / 1000:.1f} s of compiles), draws skipped while '
              f'compiling {skipped}')
        bounds = [('start', 0)] + marks + [('end', 1 << 60)]
        print(f'   {"segment":34} {"frames":>7} {"secs":>7} {">50ms":>6} {">100ms":>7} {"lost s":>7} {"worst ms":>9}')
        for (name, f0), (_, f1) in zip(bounds, bounds[1:]):
            seg = [ms for fr, ms in rows if f0 <= fr < f1]
            if not seg:
                continue
            print(f'   {name[:34]:34} {len(seg):7d} {sum(seg) / 1000:7.1f} {sum(1 for x in seg if x > 50):6d} '
                  f'{sum(1 for x in seg if x > 100):7d} {sum(x for x in seg if x > 100) / 1000:7.1f} {max(seg):9.1f}')


if __name__ == '__main__':
    main()
