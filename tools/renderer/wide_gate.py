"""lane 171: the screen-shape rule offline (docs\\renderer\\GATE_FAULTS.md rows 3 and 15, docs\\renderer\\WIDE_OURS.md).
Renders captures with rplay at a wide factor and checks the picture with numbers, 0 game runs:
  centre   the frame's middle 1/k of the width must be the 16:9 picture squeezed (the same frustum, drawn by squeezed draws): mean / p99 / over-24 pixel
           count of |wide centre - 16:9 picture resized to that width|, for the new rule and the S5 rule (--wide-legacy, the plant). Bryan's two-layer
           entrance must be far closer with the new rule; the one-layer frames must not change.
  tall     the same for a taller screen (4:3, --tall 1.3333).
  parity   16:9 with the new rplay equals the baseline rplay's picture byte for byte (--base-rplay), at scale 1 and 2.
usage: py -I tools\\renderer\\wide_gate.py [--rplay EXE] [--base-rplay EXE] [--root DIR] [--out DIR]
Exit 0 when every line passes. Captures are read from the main checkout's out\\captures and out\\cases (the same ones the other gates use)."""
import argparse, os, subprocess, sys
import numpy as np
from PIL import Image


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
    return p.returncode, p.stdout + p.stderr


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int32)


def centre_error(wide_png, base_png, k):
    w = load(wide_png)
    b = Image.open(base_png).convert('RGB')
    H, W, _ = w.shape
    cw = int(round(W / k))
    x0 = (W - cw) // 2
    ref = np.asarray(b.resize((cw, H), Image.BOX if b.size[0] > cw else Image.BILINEAR), dtype=np.int32)
    d = np.abs(w[:, x0:x0 + cw, :] - ref).max(2)
    return float(d.mean()), float(np.percentile(d, 99)), int((d > 24).sum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=None, help='the checkout whose out\\ holds the captures (default: the main checkout three folders above this worktree)')
    ap.add_argument('--rplay', default=None)
    ap.add_argument('--base-rplay', default=None)
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    main_root = os.path.normpath(os.path.join(repo, '..', '..', '..'))
    root = a.root or (main_root if os.path.isdir(os.path.join(main_root, 'out', 'captures')) else repo)
    state = {'rplay': a.rplay or os.path.join(repo, 'out', 'renderer', 'release', 'rplay.exe')}
    out = a.out or os.path.join(repo, 'out', 'l171', 'gate')
    os.makedirs(out, exist_ok=True)
    caps = {}
    for name, rel in (('bryan', os.path.join('out', 'cases', 'case_20261111_174607_799_0_pc_run_exit', 'capture')),
                      ('brock', os.path.join('out', 'captures', 'brock_s15')),
                      ('wm30', os.path.join('out', 'captures', 's100_wm30_300')),
                      ('match', os.path.join('out', 'captures', 's100_match300')),
                      ('cards', os.path.join('out', 'captures', 's100cards'))):
        p = os.path.normpath(os.path.join(root, rel))
        if os.path.isdir(p):
            caps[name] = p
    frames = {'bryan': 51, 'brock': 16, 'wm30': 2, 'match': 3, 'cards': 2}
    fails = [0]

    def line(ok, text):
        fails[0] += 0 if ok else 1
        print(('PASS ' if ok else 'FAIL ') + text, flush=True)

    def render(name, tag, extra, scale):
        png = os.path.join(out, '%s_%s_x%g.png' % (name, tag, scale))
        cmd = [state['rplay'], '--capture', caps[name], '--frames', str(frames[name]), '--scale', str(scale), '--out', png, '--root', root] + extra
        rc, text = run(cmd)
        if rc:
            raise RuntimeError('rplay %s %s failed (%d): %s' % (name, tag, rc, text[-400:]))
        return png

    for scale in (1, 2):
        for name in ('bryan', 'brock', 'wm30', 'match'):
            if name not in caps:
                line(False, 'capture %s missing' % name)
                continue
            base = render(name, '169', [], scale)
            for k, label in ((1.3125, '21:9'), (2.0, '32:9')):
                new = render(name, 'w%g' % k, ['--wide', str(k)], scale)
                leg = render(name, 'w%g_legacy' % k, ['--wide', str(k), '--wide-legacy'], scale)
                mn, pn, on = centre_error(new, base, k)
                ml, pl, ol = centre_error(leg, base, k)
                if name == 'bryan':
                    line(on * 2 < ol and mn < ml, 'centre %s %s x%g: new mean %.2f p99 %.0f over24 %d; S5 rule (the plant) mean %.2f p99 %.0f over24 %d (it draws the wrestlers unsqueezed)' % (name, label, scale, mn, pn, on, ml, pl, ol))
                else:
                    line(abs(mn - ml) < 1.0, 'centre %s %s x%g: new mean %.2f over24 %d; S5 rule mean %.2f over24 %d (one layer: equal within 1.0)' % (name, label, scale, mn, on, ml, ol))
    # a taller screen (4:3): --tall
    base = render('bryan', '169', [], 1)
    new = render('bryan', 'tall1.3333', ['--tall', '1.3333'], 1)
    leg = render('bryan', 'tall1.3333_legacy', ['--tall', '1.3333', '--wide-legacy'], 1)

    def vcentre(png):
        w = load(png)
        H = w.shape[0]
        ch = int(round(H / 1.3333))
        y0 = (H - ch) // 2
        ref = np.asarray(Image.open(base).convert('RGB').resize((w.shape[1], ch), Image.BOX), dtype=np.int32)
        d = np.abs(w[y0:y0 + ch] - ref).max(2)
        return float(d.mean()), int((d > 24).sum())

    (mn, on), (ml, ol) = vcentre(new), vcentre(leg)
    line(on * 2 < ol and mn < ml, 'centre bryan 4:3 (--tall 1.3333): new mean %.2f over24 %d; S5 rule mean %.2f over24 %d' % (mn, on, ml, ol))
    if a.base_rplay:
        for scale in (1, 2):
            for name in ('bryan', 'brock', 'cards', 'match'):
                if name not in caps:
                    continue
                n = render(name, 'par_new', [], scale)
                state['rplay'], saved = a.base_rplay, state['rplay']
                b = render(name, 'par_base', [], scale)
                state['rplay'] = saved
                line(open(n, 'rb').read() == open(b, 'rb').read(), '16:9 parity %s x%g: new == baseline byte for byte' % (name, scale))
    print('wide_gate: %s' % ('PASS' if not fails[0] else 'FAIL (%d)' % fails[0]))
    return 1 if fails[0] else 0


if __name__ == '__main__':
    sys.exit(main())
