r"""Frame replay (Session 98 S5): a dumped draw's vertices in the game's world (file space, -Y up), from its post-VS clip positions
and the view-projection rows c56..c59 of its vertex bank (clip = rows . (p, 1)); printed in the viewer's space (Y up). 0 runs.

  py -I tools\assets\replay\draw_world.py <dumped draw dir>...    -> lowest / highest / centroid of the vertices, viewer space
"""
import json
import os
import sys

import numpy as np


def world(d):
    meta = json.load(open(os.path.join(d, 'meta.json')))
    vs = np.frombuffer(open(os.path.join(d, 'cb_vs_1_1.bin'), 'rb').read(), dtype='<f4').reshape(-1, 4).astype(np.float64)
    pv = meta['postvs']
    vb = np.frombuffer(open(os.path.join(d, 'postvs.bin'), 'rb').read(), dtype='<f4')
    n = len(vb) // (pv['stride'] // 4)
    clip = vb[:n * (pv['stride'] // 4)].reshape(n, -1)[:, :4].astype(np.float64)
    R = vs[56:60]
    A = np.stack([R[0, :3], R[1, :3], R[3, :3]])
    rhs = np.stack([clip[:, 0] - R[0, 3], -clip[:, 1] - R[1, 3], clip[:, 3] - R[3, 3]], -1)  # (the translated VS writes clip y negated: host Vulkan's y-down NDC)
    p = np.linalg.solve(A, rhs.T).T
    return p * np.array([1.0, -1.0, 1.0])  # file -Y up -> viewer Y up


if __name__ == '__main__':
    for d in sys.argv[1:]:
        p = world(d)
        print('%s: %d verts, y min %.2f max %.2f, centroid %s' % (os.path.basename(d), len(p), p[:, 1].min(), p[:, 1].max(), np.round(p.mean(0), 1)))
