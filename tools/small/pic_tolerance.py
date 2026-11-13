"""Lane 135: owned_check's picture tolerance for two PNGs (tools\\native\\owned_verdict.py, docs\\renderer\\FARM.md): p99.9 of the per-pixel largest channel difference <= 8, no 16x16 block mean over 2,
share of pixels over 4 levels <= 0.3 % (the reference-noise term is 0 here: both arms are the same renderer on the same capture). Numbers only.

  py -I tools\\small\\pic_tolerance.py A.png B.png"""
import os
import sys

import numpy as np
from PIL import Image

# owned_check's own numbers, imported (never copied): tools/native/owned_verdict.py
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'native'))
from owned_verdict import TOL_SMALL, TOL_P999, TOL_BLOCK, TOL_SHARE  # noqa: E402


def load(path):
    return np.array(Image.open(path).convert('RGBA')).astype(np.int32)


def compare(a, b):
    """-> dict(max, p999, differing, over4_share, block_max, same_shape, ok)"""
    if a.shape != b.shape:
        return {'same_shape': False, 'ok': False, 'max': 255, 'p999': 255, 'differing': a.size, 'over4_share': 1.0, 'block_max': 255.0}
    d = np.abs(a[:, :, :3] - b[:, :, :3]).max(axis=2)
    h, w = d.shape
    bh, bw = h // 16, w // 16
    block = d[:bh * 16, :bw * 16].reshape(bh, 16, bw, 16).mean(axis=(1, 3)) if bh and bw else np.zeros((1, 1))
    r = {'same_shape': True, 'max': int(d.max()), 'p999': float(np.percentile(d, 99.9)), 'differing': int((d > 0).sum()), 'over4_share': float((d > TOL_SMALL).mean()), 'block_max': float(block.max())}
    r['ok'] = r['p999'] <= TOL_P999 and r['block_max'] <= TOL_BLOCK and r['over4_share'] <= TOL_SHARE
    return r


def main():
    r = compare(load(sys.argv[1]), load(sys.argv[2]))
    print(r)
    return 0 if r['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
