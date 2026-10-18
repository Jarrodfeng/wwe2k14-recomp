"""Per-scene upload / upload-skip numbers from a timeline (Session 19 item 3).

    py tools/perf/upload_stats.py RUN

From the timeline's per-frame `cpf_up` (a = upload calls, b = pages skipped, c = stale pages) and `cpf_up2`
(a = pages uploaded, b = KiB hashed, c = microseconds in the hashing loops) events, grouped into the run log's scenes
(`scene=NAME` marks, by guest frame = vdswap count). Prints means per frame."""
import csv
import re
import sys
from collections import defaultdict

L = r'C:/Users/Jarrod/wwe2k14-recomp/out/build/win-amd64-release/logs'
n = sys.argv[1].zfill(3)
marks = []
for line in open(f'{L}/wwe2k14_{n}.log', encoding='utf-8', errors='replace'):
    m = re.search(r'\[input\] line \d+(?: \([^)]*\))?: (scene=\S+|bench: done) \(frame (\d+)\)', line)
    if m:
        marks.append((int(m.group(2)), m.group(1)))
frame = 0
per = defaultdict(lambda: [0, 0, 0, 0, 0, 0])  # frame -> up calls, skipped, stale, uploaded, kib hashed, us
for r in csv.DictReader(open(f'{L}/timeline_{n}.csv', newline='')):
    try:
        e = r['event']
        if e == 'vdswap':
            frame = int(r['a'], 16)
        elif e == 'cpf_up':
            p = per[frame]; p[0] += int(r['a'], 16); p[1] += int(r['b'], 16); p[2] += int(r['c'], 16)
        elif e == 'cpf_up2':
            p = per[frame]; p[3] += int(r['a'], 16); p[4] += int(r['b'], 16); p[5] += int(r['c'], 16)
    except (ValueError, KeyError):
        break
print(f'{"scene":10} {"frames":>6} {"up calls":>9} {"skipped":>8} {"uploaded":>9} {"KiB hashed":>11} {"hash us":>8}')
for i in range(len(marks) - 1):
    f0, name = marks[i]
    f1 = marks[i + 1][0]
    if not name.startswith('scene='):
        continue
    rows = [per[f] for f in range(f0, f1) if f in per]
    k = max(1, len(rows))
    s = [sum(r[j] for r in rows) / k for j in range(6)]
    print(f'{name[6:]:10} {len(rows):6d} {s[0]:9.1f} {s[1]:8.1f} {s[3]:9.1f} {s[4]:11.0f} {s[5]:8.0f}')
