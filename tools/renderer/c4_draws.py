r"""S102 lane C4: reads rplay --draw-census 2 --draw-log lines (out\c4\draws.txt) and prints, per pass, the draws grouped by state class: blend / depth / alpha-opaque flag, with fragments and order.
  py -I tools\renderer\c4_draws.py FILE [--frame N]
Counts only (hard rule 1)."""
import re, sys, collections
RE = re.compile(r'f (\d+) p (\d+) i (\d+) (.*?) \| blend (\d+) (\d+) (\d+) (\d+) wm (\d+) dt (\d+) dw (\d+) cmp (\d+) st (\d+) alphaop (\d+) tris (\d+) area (\d+) tex (\d+) vb (\d+) vs (\d+) fs (\d+)')
def load(path):
    rows = []
    for l in open(path):
        m = RE.match(l)
        if m:
            g = m.groups()
            rows.append(dict(f=int(g[0]), p=int(g[1]), i=int(g[2]), name=g[3], blend=tuple(map(int, g[4:8])), wm=int(g[8]), dt=int(g[9]), dw=int(g[10]), cmp=int(g[11]), st=int(g[12]), ao=int(g[13]), tris=int(g[14]), area=int(g[15]), tex=int(g[16]), vb=int(g[17]), vs=int(g[18]), fs=int(g[19])))
    return rows
if __name__ == '__main__':
    path = sys.argv[1]
    frame = int(sys.argv[sys.argv.index('--frame') + 1]) if '--frame' in sys.argv else 100
    rows = [r for r in load(path) if r['f'] == frame]
    print('frame', frame, 'draws', len(rows))
    bypass = collections.defaultdict(list)
    for r in rows: bypass[r['p']].append(r)
    for p, rs in bypass.items():
        print('pass', p, 'draws', len(rs), 'fs', sum(r['fs'] for r in rs))
        cls = collections.OrderedDict()
        for r in rs:
            k = (r['blend'][0], r['blend'][1:4] if r['blend'][0] else '-', 'dt%d dw%d cmp%d st%d' % (r['dt'], r['dw'], r['cmp'], r['st']), 'ao%d' % r['ao'])
            c = cls.setdefault(k, [0, 0, 0])
            c[0] += 1; c[1] += r['fs']; c[2] += r['tris']
        for k, c in sorted(cls.items(), key=lambda kv: -kv[1][1]):
            print('  blend %s %s %s %s: draws %d fs %d tris %d' % (k + tuple(c)))
