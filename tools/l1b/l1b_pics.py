r"""S102 lane L1b: the pictures of the compute dispatch against the stream's bake, 0 game runs (fview headless).
  py -I tools\l1b\l1b_pics.py [--root DIR] [--cap bryan|brock] [--frames 20,60,85]
Per frame: the presented picture with the switch off and on (--copy-batch on, the capture-capable default bundle), every pixel compared (the whole picture, no mask). The cubes differ from the bake by
at most 1 level in a few thousandths of their values; this prints what that does to the picture: pixels differing, the largest channel difference, the share over 1 level. Exit 1 when a picture cannot be
made or the largest difference exceeds --bar (default 2 levels)."""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'l1'))
import l1_pics  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
BUNDLE = ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours', '--copy-batch', 'on']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--cap', default='bryan')
    ap.add_argument('--frames', default='20,60,85')
    ap.add_argument('--bar', type=int, default=2)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = l1_pics.main_checkout(root)
    fview = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    cap = os.path.join(main_co, 'out', 'lane86', a.cap, 'capture')
    frames = [int(x) for x in a.frames.split(',')]
    work = os.path.join(root, 'out', 'l1b', 'pics_' + a.cap)
    off, t0 = l1_pics.render_arm(fview, cap, frames, os.path.join(work, 'off'), BUNDLE)
    on, t1 = l1_pics.render_arm(fview, cap, frames, os.path.join(work, 'on'), BUNDLE + ['--prelight-compute'])
    worst = 0
    for f in frames:
        d = np.abs(off[f] - on[f]).max(axis=2)
        n = d.size
        print('frame %d: pixels differing %d of %d (%.4f %%), max %d, over 1 level: %d' % (f, int((d > 0).sum()), n, 100.0 * (d > 0).sum() / n, int(d.max()), int((d > 1).sum())))
        worst = max(worst, int(d.max()))
    print('RESULT worst=%d bar=%d' % (worst, a.bar))
    return 0 if worst <= a.bar else 1


if __name__ == '__main__':
    sys.exit(main())
