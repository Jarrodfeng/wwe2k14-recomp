"""Average fps and 1% / 5% low fps of a bench frames CSV over guest frame ranges: fps.py CSV NAME:FIRST:LAST ..."""
import csv, sys
rows = []
with open(sys.argv[1], newline='') as f:
    for r in csv.DictReader(f):
        if r['frame_ms']:
            rows.append((int(r['frame']), float(r['frame_ms'])))
for spec in sys.argv[2:]:
    name, a, b = spec.split(':')
    v = [ms for fr, ms in rows if int(a) <= fr <= int(b)]
    s = sorted(v)
    avg = 1000 * len(v) / sum(v)
    p95 = s[int(0.95 * len(s))]
    p99 = s[int(0.99 * len(s))]
    print(f'{name:8s} frames {len(v):5d}  avg {avg:5.1f} fps  5% low {1000 / p95:5.1f}  1% low {1000 / p99:5.1f}')
