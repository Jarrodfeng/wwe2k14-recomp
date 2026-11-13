"""Lane 124: the image ids of the rim light map's pass targets (site 0x82BFBD58) over frames of a capture. Numbers only."""
import sys, os, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'shadow'))
import shadow_census as sc
import numpy as np
cap = sc.Cap(sys.argv[1])
n = len(cap.frames) if hasattr(cap.frames, '__len__') else int(sys.argv[2])
step = int(sys.argv[2]) if len(sys.argv) > 2 else 1
seen = collections.Counter()
for fi in range(0, n, step):
    f = cap.frame(fi)
    if not f.has_origins:
        continue
    copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
    ps = f.origins['pass_site'].astype(np.int64)
    for p in sc.walk(f, copies):
        if p.draws and (ps[np.array(p.draws)] == sc.SITE_RIM).any():
            t = cap.imgs[p.target]
            seen[(p.target, int(t['w']), int(t['h']), int(t['format']))] += 1
print(dict(seen))
