#!/usr/bin/env python3
r"""S103 WRK check: live face wrinkles and the id-map tint in the stream-free renderer. 0 game runs: frame_rig.exe on a capture's scene record (offline; the rig's own picture).

  py -I tools\scene\wrk_check.py [--capture DIR] [--frame 110] [--out DIR] [--plant levels_dropped]

Arms (frame_rig.py --wrk-told / --wrk-off, a labelled FIXTURE: the capture is older than the writer, so no record carries the level rows the latch reads):
  A  off      the switch off (--wrk-off): a flat face, the picture before the lane.
  B  zero     the switch on, every level told 0: must EQUAL A (a level 0 draws as today: no wrinkle depth, no tint) within TOL (the wrinkle map is bound, its level is 0).
  C  levels   the switch on, Lv0..Lv5 and IdMapLv0..7 told 1: the face must CHANGE against B (>= CHANGED_PIXELS pixels differ by more than 3 / 255).
  D  twin     `--plant levels_dropped`: C is run with the switch OFF (the levels never reach the shader): the picture must equal B, the check CATCHES it (exit 0 with the token WRINKLES, as the other planted arms).
Prints one PASS / FAIL line per arm; writes DIR\wrk_sheet.png (off | zero | levels | where C differs x8).
This check needs fview / frame_rig built (tools\scene\build_rig.ps1) and the capture out\captures\brock_losswin: it is a rig-tier check (the gate counts it as skipped by default; the main session runs it).
"""
import argparse, glob, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
MAIN = os.environ.get('RDR_MAIN_CHECKOUT') or (os.path.normpath(os.path.join(ROOT, '..', '..', '..')) if os.sep + '.claude' + os.sep in ROOT else ROOT)
ENV = dict(os.environ, CLAUDECODE='1', WWE_FVIEW_ALLOW_STALE='1', RDR_MAIN_CHECKOUT=MAIN)
CHANGED_PIXELS = 20
TOL_PIXELS = 0   # the path is identical at level 0 (the wrinkle term is multiplied by the level): byte-equal
ZERO = ','.join(['0'] * 14)
FULL = ','.join(['1'] * 14)
WKL_ONLY = ','.join(['1'] * 6 + ['0'] * 8)   # the six wrinkle levels, the id-map levels left at 0


def run(cmd, timeout=1800):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=ENV, errors='replace', cwd=ROOT)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def rig(cap, frame, out, extra):
    os.makedirs(out, exist_ok=True)
    rc, text = run([sys.executable, '-I', os.path.join('tools', 'scene', 'frame_rig.py'), '--capture', cap, '--frames', str(frame), '--out', out, '--body-scale-one', '--no-sheet', '--no-family-png'] + list(extra))
    pngs = glob.glob(os.path.join(out, '**', 'ours_%d_whole.png' % frame), recursive=True)
    return rc, text, (pngs[0] if pngs else None)


def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(int)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=os.path.join(MAIN, 'out', 'captures', 'brock_losswin'))
    ap.add_argument('--frame', type=int, default=110)
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'wrk'))
    ap.add_argument('--plant', default=None, choices=[None, 'levels_dropped'])
    a = ap.parse_args()
    if not os.path.isdir(a.capture):
        print('FAIL  WRINKLES: the capture %s is not on disk' % a.capture)
        return 1
    fails = []

    def arm(name, extra):
        rc, text, png = rig(a.capture, a.frame, os.path.join(a.out, name), extra)
        if rc != 0 or not png:
            fails.append('the rig failed for %s (exit %d): %s' % (name, rc, text[-300:].replace('\n', ' ')))
            return None
        return load(png)

    zero = arm('zero', ['--wrk-told', ZERO])
    if a.plant == 'levels_dropped':
        lv = arm('plant_levels_dropped', ['--wrk-told', FULL, '--wrk-off'])
        if zero is None or lv is None:
            print('FAIL  WRINKLES (the rig itself failed, the plant is not judged): ' + '; '.join(fails))
            return 1
        n = int((np.abs(zero - lv).max(2) > 3).sum())
        if n < CHANGED_PIXELS:
            print('FAIL  WRINKLES: the face did not change when the levels never reached the shader (%d pixels < %d): the planted twin is CAUGHT (exit 0)' % (n, CHANGED_PIXELS))
            return 0
        print('PASS  WRINKLES: the planted twin was NOT caught (%d pixels changed with the switch off): the check is blind' % n)
        return 1
    off = arm('off', ['--wrk-off'])
    lv = arm('levels', ['--wrk-told', FULL])
    wk = arm('wkl_only', ['--wrk-told', WKL_ONLY])
    if any(x is None for x in (off, zero, lv, wk)):
        print('FAIL  WRINKLES: ' + '; '.join(fails))
        return 1
    nz = int((np.abs(off - zero).max(2) > 0).sum())
    ok = nz <= TOL_PIXELS
    print('%s  LEVEL 0: the switch on with every level 0 equals the flat face (%d pixels differ at all; tolerance %d)' % ('PASS' if ok else 'FAIL', nz, TOL_PIXELS))
    if not ok:
        fails.append('level 0 changed the picture')
    d = np.abs(zero - lv).max(2)
    n = int((d > 3).sum())
    ok = n >= CHANGED_PIXELS
    print('%s  LEVELS: Lv0..5 / IdMapLv0..7 told 1 change the face (%d pixels differ from the level-0 picture; bar %d)' % ('PASS' if ok else 'FAIL', n, CHANGED_PIXELS))
    if not ok:
        fails.append('levels did not change the face')
    # the wrinkle levels alone (id-map 0) must change pixels in the HEAD: the top fifth of the figure's box (the figure = where the all-levels picture differs from the flat one)
    dw = np.abs(zero - wk).max(2)
    ys, xs = np.nonzero(d > 3)
    if len(ys):
        top, bot = int(ys.min()), int(ys.max())
        head = int((dw[top:top + max(1, (bot - top) // 5 + 1)] > 3).sum())
        ok = head >= 1
        print('%s  WRINKLES ONLY: the six wrinkle levels told 1 (id-map 0) change %d pixels in the head band (top fifth of the figure, rows %d-%d), %d in all' % ('PASS' if ok else 'FAIL', head, top, top + (bot - top) // 5, int((dw > 3).sum())))
        if not ok:
            fails.append('wrinkles alone did not change the head')
    try:
        w, h = zero.shape[1], zero.shape[0]
        diff = np.clip(d * 8, 0, 255).astype(np.uint8)
        sheet = Image.new('RGB', (w * 4, h + 18), (20, 20, 20))
        for i, (img, label) in enumerate(((off, 'switch off (flat)'), (zero, 'on, levels 0'), (lv, 'on, levels 1'), (np.stack([diff] * 3, 2), 'where levels differ x8'))):
            sheet.paste(Image.fromarray(img.astype(np.uint8)), (i * w, 18))
            ImageDraw.Draw(sheet).text((i * w + 4, 2), label, fill=(255, 255, 255))
        sheet.save(os.path.join(a.out, 'wrk_sheet.png'))
    except Exception as e:  # noqa: BLE001
        print('(sheet not written: %s)' % e)
    if fails:
        print('FAIL  WRINKLES: ' + '; '.join(fails))
        return 1
    print('PASS  WRINKLES: level 0 draws as today, non-zero levels change the face')
    return 0


if __name__ == '__main__':
    sys.exit(main())
