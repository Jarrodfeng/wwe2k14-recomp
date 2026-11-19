r"""L1: per-frame histogram on a capture (find the frames the bars fail on). py -I tools\l1\l1_perframe.py CAP STEP [EXTRA...]"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402
import l1_pics  # noqa: E402

root = os.path.normpath(os.path.join(HERE, '..', '..'))
main = l1_pics.main_checkout(root)
cap = sys.argv[1]
step = int(sys.argv[2])
fview = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
c = os.path.join(main, 'out', 'lane86', cap, 'capture')
n = int(sys.argv[3]) if len(sys.argv) > 3 else 341
frames = list(range(0, n, step))
work = os.path.join(root, 'out', 'l1', 'pf_' + cap)
base = ['--copy-batch', 'on']
off, _ = l1_pics.render_arm(fview, c, frames, os.path.join(work, 'off'), base)
on, _ = l1_pics.render_arm(fview, c, frames, os.path.join(work, 'on'), base + ['--prelight-analytic'])
drop, _ = l1_pics.render_arm(fview, c, frames, os.path.join(work, 'drop'), base + ['--draw-drop', '@class=wrestlers'])
for f in frames:
    mask = np.abs(off[f] - drop[f]).max(axis=2) > 0
    s = l1_pics.diff_stats(off[f], on[f], mask)
    print('frame %3d px %8d mean %6.3f p99.9 %4.0f gt12 %7.3f%% max %3d outside %d' % (f, s['pixels'], s['mean'], s['p999'], 100 * s['gt12'], s['max'], s['outside']))
