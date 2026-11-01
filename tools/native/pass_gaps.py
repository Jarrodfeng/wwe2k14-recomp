"""Session 80 (frame structure): one pass-log frame's segments by kind (4 pass, 6 resolve, 7 fill, 8-10 the deferred
batch, 11 its dispatches; the batch kinds with gpu_ram_stats), the biggest gaps before a segment and the segments over
0.5 ms. Input: the frame's [native][pass] lines with everything up to '[pass] ' cut, e.g.

    Select-String -Path LOG -Pattern '\\[native\\]\\[pass\\] frame 5830 #' | ForEach-Object { ... } | Set-Content f.txt
    py -I tools\\native\\pass_gaps.py f.txt [N]
"""
import re
import sys

rx = re.compile(r'#(\d+) kind (\d+) (\d+)x(\d+) pitch \d+ colour kind (\d+) depth (\d+) draws (\d+) vs (\w+) ps (\w+): '
                r'([0-9.]+) ms \(before it ([0-9.]+) ms\)')
raw = open(sys.argv[1], 'rb').read()
text = raw.decode('utf-16') if raw[:2] == b'\xff\xfe' else raw.decode('utf-8', 'replace')
rows = []
for line in text.splitlines():
    m = rx.search(line)
    if m:
        rows.append(dict(i=int(m[1]), kind=int(m[2]), w=int(m[3]), h=int(m[4]), ck=int(m[5]), d=int(m[6]), draws=int(m[7]),
                         vs=m[8][:8], ps=m[9][:8], ms=float(m[10]), gap=float(m[11])))
by_kind = {}
for r in rows:
    k = by_kind.setdefault(r['kind'], [0, 0.0, 0.0])
    k[0] += 1
    k[1] += r['ms']
    k[2] += r['gap']
print('kind: count, ms in, gap before')
for k, v in sorted(by_kind.items()):
    print(f'  {k}: {v[0]:4d} {v[1]:7.2f} {v[2]:7.2f}')
print('total in', round(sum(r['ms'] for r in rows), 2), 'gaps', round(sum(r['gap'] for r in rows), 2))
print('biggest gaps (prev segment -> this segment):')
n = int(sys.argv[2]) if len(sys.argv) > 2 else 20
by_index = {r['i']: r for r in rows}
for r in sorted(rows, key=lambda r: -r['gap'])[:n]:
    p = by_index.get(r['i'] - 1)
    ps = f"#{p['i']} k{p['kind']} {p['w']}x{p['h']} d{p['draws']} {p['ps']} {p['ms']:.2f}" if p else '-'
    print(f"  {r['gap']:6.2f} before #{r['i']} k{r['kind']} {r['w']}x{r['h']} draws {r['draws']} vs {r['vs']} ps {r['ps']} "
          f"({r['ms']:.2f} ms)  after {ps}")
print('segments > 0.5 ms:')
for r in rows:
    if r['ms'] > 0.5:
        print(f"  #{r['i']} k{r['kind']} {r['w']}x{r['h']} draws {r['draws']} vs {r['vs']} ps {r['ps']}: {r['ms']:.2f} "
              f"(gap {r['gap']:.2f})")
