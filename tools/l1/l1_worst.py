"""L1: where are the largest differences (reads the PNGs l1_pics.py left in out/l1/pics_<cap>/{off,on}/)."""
import os
import sys

import numpy as np
from PIL import Image

cap = sys.argv[1] if len(sys.argv) > 1 else 'bryan'
frame = int(sys.argv[2]) if len(sys.argv) > 2 else 85
base = os.path.join('out', 'l1', 'pics_' + cap)
off = np.array(Image.open(os.path.join(base, 'off', 'frame_%05d.png' % frame)).convert('RGB')).astype(int)
on = np.array(Image.open(os.path.join(base, 'on', 'frame_%05d.png' % frame)).convert('RGB')).astype(int)
d = np.abs(off - on).max(axis=2)
ys, xs = np.nonzero(d > 12)
print('pixels > 12:', len(ys))
order = np.argsort(-d[ys, xs])[:12]
for i in order:
    y, x = ys[i], xs[i]
    print('  (%d,%d) off %s on %s' % (x, y, off[y, x].tolist(), on[y, x].tolist()))
