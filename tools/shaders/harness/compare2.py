import numpy as np, sys
a = np.fromfile(sys.argv[1], np.float32).reshape(-1, 4)
b = np.fromfile(sys.argv[2], np.float32).reshape(-1, 4)
d = np.abs(a - b); rel = (d / np.maximum(np.abs(b), 1e-3)).max(1)
idx = np.arange(len(a))
bad = rel > 1e-3
for m in range(3):
    s = (idx % 3 == m)
    print('idx%3==', m, 'bad', int(bad[s].sum()), 'of', int(s.sum()))
for par in range(2):
    s = (idx % 2 == par)
    print('idx%2==', par, '(paint branch %s)' % ('off' if par == 0 else 'on'), 'bad', int(bad[s].sum()), 'of', int(s.sum()))
s = (idx % 3 != 1)
print('literal bank as guessed: bad', int(bad[s].sum()), 'of', int(s.sum()), 'max rel', rel[s].max())
