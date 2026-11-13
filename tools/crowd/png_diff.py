r"""Lane 155 (S101): how much two PNGs of the same size differ. Numbers only.
  py -I tools\crowd\png_diff.py A.png B.png [--tol N]
Prints: pixels differing at all, pixels off by more than N levels in any channel (default 8), the largest channel difference, the bounding box of the differing pixels. Exit 0."""
import sys

import numpy as np
from PIL import Image

a = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(int)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB')).astype(int)
tol = int(sys.argv[sys.argv.index('--tol') + 1]) if '--tol' in sys.argv else 8
if a.shape != b.shape:
    print('sizes differ: %s vs %s' % (a.shape, b.shape))
    sys.exit(1)
d = np.abs(a - b).max(axis=2)
ys, xs = np.nonzero(d)
print('%dx%d: %d pixels differ (%.4f%%), %d by more than %d levels, max channel difference %d%s' % (
    a.shape[1], a.shape[0], len(ys), 100.0 * len(ys) / d.size, int((d > tol).sum()), tol, int(d.max()),
    ('; bounding box x %d..%d y %d..%d' % (xs.min(), xs.max(), ys.min(), ys.max())) if len(ys) else ''))
