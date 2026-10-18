"""Per-thread CPU per scene from tools/perf/thread_busy.ps1 output (Session 19 item 1).

    py tools/perf/thread_report.py RUN [--top 20]

Scenes come from the run log's script marks (`scene=NAME` ... next mark / `bench: done`), matched to the sampler's
wall-clock rows. Per scene and thread (grouped by name; unnamed threads by tid): CPU % of one core, and ms per guest
frame (CPU ms / frames presented in the scene). Spinning threads (main, GPU Commands) read ~100% by design."""
import argparse
import csv
import re
from collections import defaultdict
from datetime import datetime

L = r'C:/Users/Jarrod/wwe2k14-recomp/out/build/win-amd64-release/logs'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--top', type=int, default=20)
    a = ap.parse_args()
    n = a.run.zfill(3)
    marks = []
    for line in open(f'{L}/wwe2k14_{n}.log', encoding='utf-8', errors='replace'):
        m = re.match(r'\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*\[input\] line \d+(?: \([^)]*\))?: (scene=\S+|bench: done)'
                     r'.*\(frame (\d+)\)', line)
        if m:
            marks.append((datetime.strptime(m.group(1)[:23], '%Y-%m-%d %H:%M:%S.%f'), m.group(2), int(m.group(3))))
    day = marks[0][0].date() if marks else None
    rows = []
    for r in csv.DictReader(open(f'{L}/threads_{n}.csv', newline='')):
        t = datetime.combine(day, datetime.strptime(r['time'], '%H:%M:%S.%f').time())
        rows.append((t, r['tid'], r['name'], float(r['cpu_ms']), float(r['interval_ms'])))
    for i in range(len(marks) - 1):
        t0, name, f0 = marks[i]
        t1, _, f1 = marks[i + 1]
        if not name.startswith('scene='):
            continue
        cpu = defaultdict(float)
        span = defaultdict(float)
        for t, tid, tname, ms, iv in rows:
            if t0 < t <= t1:
                key = tname if tname != '-' else f'tid {tid}'
                cpu[key] += ms
                span[key] = max(span[key], 0)
        secs = (t1 - t0).total_seconds()
        frames = max(1, f1 - f0)
        print(f'== {name[6:]}: {secs:.1f} s, {frames} frames ({frames / secs:.1f} fps)')
        for key, ms in sorted(cpu.items(), key=lambda kv: -kv[1])[:a.top]:
            print(f'  {key:28} {100 * ms / 1000 / secs:6.1f}%  {ms / frames:6.2f} ms/frame')


if __name__ == '__main__':
    main()
