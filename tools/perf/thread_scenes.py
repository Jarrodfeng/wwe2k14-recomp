"""Every thread's CPU time per scene, from tools\\perf\\thread_busy.ps1's CSV and the run log's scene marks (Session 79).

The marks are the route's "log scene=NAME" lines (profile_lesnar_bryan: tron_343, tron_324, match) up to "bench:
done". Each one-second sample counts for the scene it started in (samples that cross a mark are left out). CPU ms per
second / 60 = ms per 60 fps frame; "all threads" is the whole process.

  py tools\\perf\\thread_scenes.py THREADS_CSV RUN_LOG [--top N]
"""
import argparse
import csv
import re
from collections import defaultdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('threads_csv')
    ap.add_argument('run_log')
    ap.add_argument('--top', type=int, default=12)
    a = ap.parse_args()
    marks = []
    for line in open(a.run_log, encoding='utf-8', errors='replace'):
        m = re.match(r'\[\d{4}-\d\d-\d\d (\d\d):(\d\d):(\d\d\.\d+)\].*\[wwe2k14\]\[input\] line \d+: (scene=\w+|bench: done)',
                     line)
        if m:
            t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3))
            marks.append((t, m.group(4).replace('scene=', '')))

    def scene_at(t):
        name = None
        for mt, mn in marks:
            if t >= mt:
                name = mn
        return name

    per = defaultdict(lambda: defaultdict(float))
    secs = defaultdict(float)
    seen = set()
    for row in csv.DictReader(open(a.threads_csv, encoding='utf-8')):
        h, mi, s = row['time'].split(':')
        t = int(h) * 3600 + int(mi) * 60 + float(s)
        start = t - float(row['interval_ms']) / 1000.0
        sc = scene_at(start)
        if sc is None or sc == 'bench: done' or scene_at(t) != sc:
            continue
        if (row['time'], sc) not in seen:
            seen.add((row['time'], sc))
            secs[sc] += float(row['interval_ms']) / 1000.0
        name = row['name'] if row['name'] != '-' else f"(unnamed tid {row['tid']})"
        per[sc][name] += float(row['cpu_ms'])
    for sc in [m[1] for m in marks if m[1] != 'bench: done']:
        if sc not in per or not secs[sc]:
            continue
        total = sum(per[sc].values()) / secs[sc] / 60.0
        print(f'{sc}: {secs[sc]:.0f} s sampled, all threads {total:.2f} ms CPU per 60 fps frame')
        for name, ms in sorted(per[sc].items(), key=lambda kv: -kv[1])[:a.top]:
            v = ms / secs[sc] / 60.0
            if v < 0.05:
                break
            print(f'   {v:6.2f} ms  {name}')


if __name__ == '__main__':
    main()
