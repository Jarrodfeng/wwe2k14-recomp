"""lane 177 (Session 101): full-screen fades / flashes / tints at 21:9 and 32:9, offline (rplay --wide), 0 game runs.
The Brock entrance capture (out\\captures\\brock_s15_300, 16 frames) carries a real full-view T_Rect every frame: Color (1, 1, 0.784, ~0.35-0.42), corners (0,0)-(1280,720).
Frame 15 is rendered at a wide factor k three ways: `drop` (--wide-drop-fullview: the full-view quad writes nothing, an unflashed baseline), `new` (the rule of
this lane) and `old` (--wide-no-fullview: the lane 171 behaviour, the plant). The flash is  X - drop  (luma, per column):
  cover    the share of the frame width whose columns carry the flash (at least a quarter of the median flash in the middle 1/k)
           new: 100 %.  old (the plant): the middle 1/k only (76.2 % at 21:9, 50 % at 32:9)
  sides    the luma the flash adds to the left / right strips (outside the middle 1/k): new > 5 levels, old 0
  middle   the middle 1/k carries the same flash in both
  (the flash is additive onto the picture, so its size follows what is under it: no evenness test)
  parity   16:9 (no --wide) with this rplay equals the baseline rplay's picture byte for byte (--base-rplay)
usage: py -I tools\\renderer\\fullview_gate.py [--rplay EXE] [--base-rplay EXE] [--root DIR] [--out DIR]   exit 0 = every line passes"""
import argparse, os, subprocess, sys
import numpy as np
from PIL import Image


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
    return p.returncode, p.stdout + p.stderr


def lum(path):
    im = np.asarray(Image.open(path).convert('RGB'), dtype=np.float64)
    return 0.2126 * im[..., 0] + 0.7152 * im[..., 1] + 0.0722 * im[..., 2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=None)
    ap.add_argument('--rplay', default=None)
    ap.add_argument('--base-rplay', default=None)
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    main_root = os.path.normpath(os.path.join(repo, '..', '..', '..'))
    root = a.root or (main_root if os.path.isdir(os.path.join(main_root, 'out', 'captures')) else repo)
    rplay = a.rplay or os.path.join(repo, 'out', 'renderer', 'release', 'rplay.exe')
    out = a.out or os.path.join(repo, 'out', 'l177', 'gate')
    os.makedirs(out, exist_ok=True)
    cap = os.path.join(root, 'out', 'captures', 'brock_s15_300')
    fails = [0]

    def line(ok, text):
        fails[0] += 0 if ok else 1
        print(('PASS ' if ok else 'FAIL ') + text, flush=True)

    def render(tag, extra, scale, exe=None):
        png = os.path.join(out, 'br_%s_x%g.png' % (tag, scale))
        rc, text = run([exe or rplay, '--capture', cap, '--frames', '16', '--scale', str(scale), '--out', png, '--root', root] + extra)
        if rc:
            raise RuntimeError('rplay %s failed (%d): %s' % (tag, rc, text[-300:]))
        return png

    for scale in (1, 2):
        for k, label in ((1.3125, '21:9'), (2.0, '32:9')):
            drop = lum(render('drop_%g' % k, ['--wide', str(k), '--wide-drop-fullview'], scale))
            new = lum(render('new_%g' % k, ['--wide', str(k)], scale))
            old = lum(render('old_%g' % k, ['--wide', str(k), '--wide-no-fullview'], scale))
            H, W = new.shape
            cw = int(round(W / k))
            x0 = (W - cw) // 2
            x1 = x0 + cw
            m = 4 * scale
            flash_new = (new - drop).mean(axis=0)  # per column, luma added by the flash
            flash_old = (old - drop).mean(axis=0)
            ref = float(np.median(flash_new[x0 + m:x1 - m]))
            cov_new = 100.0 * float(np.mean(flash_new > 0.25 * ref))
            cov_old = 100.0 * float(np.mean(flash_old > 0.25 * ref))
            side = np.r_[np.arange(2, x0 - m), np.arange(x1 + m, W - 2)]
            sides_new, sides_old = float(flash_new[side].mean()), float(flash_old[side].mean())
            mid_diff = float(np.abs(flash_new[x0 + m:x1 - m] - flash_old[x0 + m:x1 - m]).mean())
            uni = float(np.mean(np.abs(flash_new[2:W - 2] - ref) <= 0.10 * ref + 1.0))
            ok = ref > 10 and cov_new > 99.0 and abs(cov_old - 100.0 / k) < 3.0 and sides_new > 5 and sides_old < 1 and mid_diff < 1
            line(ok, 'brock flash %s x%g: flash luma in the middle %.1f; coverage of the frame width new %.1f%% (plant: lane 171 behaviour %.1f%%, middle-only would be %.1f%%); luma added on the side strips new %.1f, plant %.1f; middle new vs plant differs %.2f (columns evenly lit: %.0f%%, informational)'
                 % (label, scale, ref, cov_new, cov_old, 100.0 / k, sides_new, sides_old, mid_diff, 100.0 * uni))
    if a.base_rplay:
        for scale in (1, 2):
            n = render('par_new', [], scale)
            b = render('par_base', [], scale, a.base_rplay)
            line(open(n, 'rb').read() == open(b, 'rb').read(), '16:9 parity brock x%g: this rplay == baseline rplay byte for byte (the rule is unreachable at k = 1)' % scale)
    print('fullview_gate: %s' % ('PASS' if not fails[0] else 'FAIL (%d)' % fails[0]))
    return 1 if fails[0] else 0


if __name__ == '__main__':
    sys.exit(main())
