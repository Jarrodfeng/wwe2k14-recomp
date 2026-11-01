import numpy as np, sys
a = np.fromfile(sys.argv[1], np.float32).reshape(-1, 4)   # ours
b = np.fromfile(sys.argv[2], np.float32).reshape(-1, 4)   # reference
d = np.abs(a - b); rel = (d / np.maximum(np.abs(b), 1e-3)).max(1)
idx = np.arange(len(a))
print('records', len(a), 'non-finite ours', int((~np.isfinite(a)).any(1).sum()), 'ref', int((~np.isfinite(b)).any(1).sum()))
print('max abs diff', float(d.max()), 'max rel (floor 1e-3)', float(rel.max()), 'median rel', float(np.median(rel)))
bad = rel > 1e-3
for m in range(3):
    s = (idx % 3 == m)
    print('idx%3==', m, '(%s)' % ('random literal bank' if m == 1 else 'literal bank as expected'), 'bad(>1e-3)', int(bad[s].sum()), 'of', int(s.sum()), 'max rel', float(rel[s].max()))
for par in range(2):
    s = (idx % 2 == par)
    print('idx%2==', par, '(paint branch %s)' % ('off' if par == 0 else 'on'), 'bad', int(bad[s].sum()), 'of', int(s.sum()), 'max rel', float(rel[s].max()))
sp = (idx % 7 == 3)
print('special (vertical normal, eps 0):', 'bad', int(bad[sp].sum()), 'of', int(sp.sum()), 'max rel', float(rel[sp].max()))
print('fraction under 1e-5:', float((rel < 1e-5).mean()), ' under 1e-6:', float((rel < 1e-6).mean()))
