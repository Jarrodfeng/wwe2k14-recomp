#!/usr/bin/env python3
r"""S103 lane LAY check: the UVScroll layers 2..4 (stage-side LED equaliser panels, arena 4 slot 226 `otl_titan05`).

  py -I tools\scene\lay_check.py [--capture out\captures\a04_entrance] [--frame 60] [--out DIR]

Offline (0 game runs): frame_rig on the capture's frame twice, with the layers (the build) and with the planted twin WWE_RDR_PLANT_ONE_LAYER=1 (the old one-layer draw:
only the red / black band texture `otl_titan05_1` / `_2`, drawn as big red / black blocks). The panel is the red-dominant blob of the planted picture (x >= 500, the stage-side
panel); the measure is the share of its pixels that are bright red-dominant (r > 100, r > 2 g, r > 2 b); measured on a04_entrance f60: layers 0.144, planted 0.371: the one-layer picture is mostly red (the bands), the real one is a dark
panel with a few red / blue LED bars (the LED texture `otl_titan05_3` is opaque dark where the LEDs are off, and the bands show through only in its pockets).
PASS needs: layers share <= half the planted share, planted share >= 0.30 (the twin must FAIL the same bar), and the layer picture differs from the planted one.
"""
import argparse, contextlib, io, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import run_dirs  # noqa: E402   (S103 PSO: a private output folder per run)
import frame_rig  # noqa: E402   (S103 PS1: both arms on a frame_rig.exe --serve session: the game's replay picture, body scales and camera are made once; each arm's process loads the game once)


def rig(capture, frame, out, plant):
    os.environ['WWE_FVIEW_ALLOW_STALE'] = '1'   # the game side (fview) is not under test
    os.environ.pop('WWE_RDR_PLANT_ONE_LAYER', None)
    if plant:
        os.environ['WWE_RDR_PLANT_ONE_LAYER'] = '1'   # a front-end option of the rig process: the session keeps one server per such environment
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        frame_rig.main(['--capture', capture, '--frames', str(frame), '--repeat', '1', '--out', out, '--no-sheet', '--no-family-png'])   # (only the whole picture is read)
    os.environ.pop('WWE_RDR_PLANT_ONE_LAYER', None)
    pic = os.path.join(out, 'work_' + os.path.basename(capture.rstrip('\\/')), 'ours_%d_whole.png' % frame)
    if not os.path.exists(pic):
        print(buf.getvalue()[-1200:])
        raise SystemExit('FAIL  no picture from the rig')
    return np.array(Image.open(pic).convert('RGB')).astype(int)


def reddom(a):
    return (a[..., 0] > 100) & (a[..., 0] > 2 * a[..., 1]) & (a[..., 0] > 2 * a[..., 2])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=os.path.join(ROOT, 'out', 'captures', 'a04_entrance'))
    ap.add_argument('--frame', type=int, default=60)
    ap.add_argument('--out', default=None)
    ap.add_argument('--plant', default='', help='one_layer: judge the planted one-layer picture as if it were the build; exits 0 only when the check FAILS it')
    a = ap.parse_args()
    a.out = run_dirs.run_out('lay', a.out)   # --out, else a per-run folder (removed on PASS)
    cap = a.capture if os.path.isdir(a.capture) else os.path.join(os.environ.get('RDR_MAIN_CHECKOUT', ROOT), 'out', 'captures', 'a04_entrance')
    frame_rig.SESSION = frame_rig.RigSession()
    try:
        on = rig(cap, a.frame, os.path.join(a.out, 'on'), False)
        pl = rig(cap, a.frame, os.path.join(a.out, 'plant'), True)
    finally:
        frame_rig.SESSION.close()
    m = reddom(pl)
    win = np.zeros_like(m)
    win[140:230, 560:900] = True   # the stage-side panel of a04_entrance f60 (the rig is 1280 x 720)
    m &= win
    ys, xs = np.nonzero(m)
    if len(xs) < 200:
        raise SystemExit('FAIL  the planted picture has no panel (%d red-dominant pixels)' % len(xs))
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    box = (slice(y0, y1), slice(x0, x1))
    share_on = reddom(on[box]).mean()
    share_pl = reddom(pl[box]).mean()
    diff = np.abs(on - pl).mean()
    print('panel box x %d..%d y %d..%d; bright red-dominant share: layers %.3f, planted one-layer %.3f; mean |diff| %.2f' % (x0, x1, y0, y1, share_on, share_pl, diff))
    ok = True
    judged = share_pl if a.plant == 'one_layer' else share_on   # the plant arm judges the planted picture against the same absolute bar
    if not judged <= 0.22:
        ok = False
        print('FAIL  layers share %.3f > 0.22 (measured layers 0.144): the LED layer is not over the bands' % judged)
    if not share_on <= 0.5 * share_pl and a.plant != 'one_layer':
        ok = False
        print('FAIL  layers share %.3f > half of the planted one: the LED layer is not over the bands' % share_on)
    if not share_pl >= 0.30:
        ok = False
        print('FAIL  the planted twin share %.3f < 0.30: the one-layer picture is not the checker' % share_pl)
    if not diff > 0.05 and a.plant != 'one_layer':
        ok = False
        print('FAIL  the picture does not change with the plant')
    if a.plant:
        print('PASS  plant caught (the check fails the one-layer picture)' if not ok else 'FAIL  the plant was NOT caught')
        return run_dirs.finish(a.out, 0 if not ok else 1)
    print('PASS  lay_check' if ok else 'FAIL  lay_check')
    return run_dirs.finish(a.out, 0 if ok else 1)


if __name__ == '__main__':
    sys.exit(main())
