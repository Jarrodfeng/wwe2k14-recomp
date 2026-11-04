"""Average fps and 1% / 5% low fps of a bench frames CSV over guest frame ranges: fps.py CSV NAME:FIRST:LAST ...
Session 89: also prints the run's seed from the log beside the CSV (frames_NNN.csv -> wwe2k14_NNN.log): Bryan's entrance
has two camera variants and only a fixed seed (--wwe2k14_fixed_seed) makes two runs' Bryan numbers comparable."""
import csv, re, sys
from pathlib import Path

rows = []
with open(sys.argv[1], newline='') as f:
    for r in csv.DictReader(f):
        if r['frame_ms']:
            rows.append((int(r['frame']), float(r['frame_ms'])))
m = re.search(r'frames_(\d+)\.csv$', sys.argv[1])
log = Path(sys.argv[1]).with_name(f'wwe2k14_{m.group(1)}.log') if m else None
seed = None
if log and log.exists():
    with open(log, encoding='utf-8', errors='replace') as f:
        for i, line in enumerate(f):
            s = re.search(r'\[wwe2k14\]\[seed\] fixed seed (\d+)', line)
            if s:
                seed = s.group(1); break
            if i > 20000:
                break
    print(f'seed: fixed {seed} (camera variants comparable with other seed-{seed} runs)' if seed else
          'seed: NOT FIXED - Bryan\'s camera variant is not pinned; compare his numbers only with care')
else:
    print('seed: unknown (no wwe2k14_NNN.log beside the CSV)')
for spec in sys.argv[2:]:
    name, a, b = spec.split(':')
    v = [ms for fr, ms in rows if int(a) <= fr <= int(b)]
    s = sorted(v)
    avg = 1000 * len(v) / sum(v)
    p95 = s[int(0.95 * len(s))]
    p99 = s[int(0.99 * len(s))]
    print(f'{name:8s} frames {len(v):5d}  avg {avg:5.1f} fps  5% low {1000 / p95:5.1f}  1% low {1000 / p99:5.1f}')
