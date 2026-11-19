#!/usr/bin/env python3
r"""Lane SCR (Session 103): the movie-surface rule runs in the live loader (the ghost floor decal of the stage screens), offline (0 game runs).

  py -I tools\scene\scr_check.py [--capture C:\...\out\captures\a04_entrance] [--frame 60] [--plant] [--expect-fail] [--out DIR]

The screens' floor reflection copy (key 805 / 808: flat at the floor, stored vertex alpha 0..17 of 255) must be a faint reflection (SrcAlpha / InvSrcAlpha by the vertex alpha), not an
opaque white band under the wall. The rig draws the frame's SCREENS panel twice: with the rule (the loader's) and with WWE_RDR_PLANT_NO_SCREEN_RULE=1 (the old viewer-only rule: rf::c::Plants::no_screen_rule).
  ghost region = the pixels where the planted picture is brighter than the ruled one by more than 10 % of the wall's brightness M (M = the 90th percentile of the panel's excess over the clear colour).
  1. the region exists (>= 300 px): the plant really draws a band (else the check proves nothing).
  2. the SUBJECT picture's mean excess over that region <= 0.10 x M (the floor copy is at most ~7 % of the wall's brightness; bar 10 % allows the clear-colour blend).
  --plant makes the subject the planted picture: it must FAIL 2 (--expect-fail turns that into the exit code). Info: the count of bright pixels per row below the wall.
"""
import argparse, os, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CLEAR = np.array([31, 36, 46], dtype=np.float64)


def rig(capture, frame, out, plant):
    env = dict(os.environ)
    env['WWE_FVIEW_ALLOW_STALE'] = '1'
    env.pop('WWE_RDR_PLANT_NO_SCREEN_RULE', None)
    if plant:
        env['WWE_RDR_PLANT_NO_SCREEN_RULE'] = '1'
    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'frame_rig.py'), '--capture', capture, '--frames', str(frame), '--repeat', '1', '--out', out],
                       capture_output=True, text=True, errors='replace', env=env, timeout=1800)
    f = os.path.join(out, 'work_' + os.path.basename(os.path.normpath(capture)), 'ours_%d_screens.png' % frame)
    if not os.path.exists(f):
        print('FAIL  rig run gave no screens panel (%s): %s' % (f, (p.stdout or '')[-600:]))
        sys.exit(1)
    return np.asarray(Image.open(f).convert('RGB'), dtype=np.float64)


def excess(a):
    return np.abs(a - CLEAR).sum(-1) / 3.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=None)
    ap.add_argument('--frame', type=int, default=60)
    ap.add_argument('--plant', action='store_true')
    ap.add_argument('--expect-fail', action='store_true')
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    cap = a.capture
    if not cap:
        sys.path.insert(0, HERE)
        import frame_rig
        cap = os.path.join(frame_rig.main_checkout(), 'out', 'captures', 'a04_entrance')
    out = a.out or os.path.join(ROOT, 'out', 'scr')
    ruled = excess(rig(cap, a.frame, os.path.join(out, 'fix'), False))
    planted = excess(rig(cap, a.frame, os.path.join(out, 'plant'), True))
    if ruled.shape != planted.shape:
        print('FAIL  panel sizes differ')
        return 1
    M = float(np.percentile(ruled[ruled > 5], 90)) if (ruled > 5).any() else 0.0
    ghost = (planted - ruled) > 0.1 * M
    n = int(ghost.sum())
    fails = []
    print('  wall brightness M %.1f (90th percentile of the ruled panel); ghost region (planted brighter by > 0.1 M): %d px' % (M, n))
    if n < 300:
        fails.append('ghost region only %d px (the planted twin draws no band: nothing to prove)' % n)
        print('FAIL  1: %s' % fails[-1])
        return 1
    print('PASS  1: the planted picture draws a band of %d px under / around the wall' % n)
    subj = planted if a.plant else ruled
    mean = float(subj[ghost].mean()) / M
    ok = mean <= 0.10
    print('%s  2: %s picture over the ghost region: mean excess %.3f x M (bar 0.10)' % ('PASS' if ok else 'FAIL', 'PLANTED' if a.plant else 'ruled', mean))
    if not ok:
        fails.append('floor copy at %.2f of the wall' % mean)
    if a.expect_fail:
        print('%s  planted: %s' % ('PASS' if fails else 'FAIL', 'failed as it must (%s)' % '; '.join(fails) if fails else 'did NOT fail'))
        return 0 if fails else 1
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
