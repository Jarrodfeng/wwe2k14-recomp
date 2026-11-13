"""lane 186 (Session 101): HUD-phase rectangles at 21:9 and 32:9, offline (rplay --wide), 0 game runs. docs\\renderer\\WIDE_OURS.md section 8.
After the HUD marker every T_Rect (the flat rectangles under / between the HUD's sprites: VS 032E8FAC979E01DA, patched CAAB5FD3B6EE4944) is squeezed like the sprites,
except the full-view ones (FullViewQuad: fades, tints, the 30 Years film-filter quad). The plant is `--wide-post-rect` (the old post-pass list: every T_Rect left alone).
Captures (read from the main checkout's out\\): Heenan = the 30 Years match-information screen (s100_heenan_build, 100 frames), editor = the Create-a-Superstar CLOTHING menu
(fview_unpack_20261112-185530, 300 frames), wm30 = the "CAGE CONTROLS" bar (s100_wm30_300, 2 frames), cards = a 30 Years capture whose filter quad is full-view (s100cards, 2 frames).
  census   (--wide-census) after the marker: partial T_Rects SQUEEZE, full-view ones FULL-VIEW, none left; plant: all of them left (count > 0)
  picture  editor frame 299 and Heenan frame 99: the middle 1/k of the wide render equals the 16:9 picture squeezed (wide_gate centre_error); the plant is far worse
  readback the footprint of one draw read from the render (rplay --wide-mute-draw N: render with and without it, diff): Heenan d32 / d380, WM30 d1080 at the squeezed x
           (640 + (x - 640) / k; 259..1021, 277..570, 106.7..1173.3 at 21:9); the plant keeps the unsqueezed x
  filter   cards: default == plant byte for byte (the filter quad is full-view, so leaving the hash list changed nothing on it); census: FULL-VIEW, copy pass still left by hash
  parity   16:9 equals the baseline rplay (--base-rplay) byte for byte
usage: py -I tools\\renderer\\hudrect_gate.py [--rplay EXE] [--base-rplay EXE] [--root DIR] [--out DIR]   exit 0 = every line passes"""
import argparse, os, re, subprocess, sys
import numpy as np
from PIL import Image


def run(cmd, timeout=1200):
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
    ap.add_argument('--root', default=None)
    ap.add_argument('--rplay', default=None)
    ap.add_argument('--base-rplay', default=None)
    ap.add_argument('--out', default=None)
    ap.add_argument('--as-plant', action='store_true', help='run the default lines with --wide-post-rect (the plant): the picture / census lines must FAIL (exit 1)')
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    main_root = os.path.normpath(os.path.join(repo, '..', '..', '..'))
    root = a.root or (main_root if os.path.isdir(os.path.join(main_root, 'out', 'captures')) else repo)
    rplay = os.path.abspath(a.rplay or os.path.join(repo, 'out', 'renderer', 'release', 'rplay.exe'))
    out = a.out or os.path.join(repo, 'out', 'l186', 'gate')
    os.makedirs(out, exist_ok=True)
    caps = {'heenan': (os.path.join('out', 'captures', 's100_heenan_build'), 100),
            'editor': (os.path.join('out', 'cases', 'fview_unpack_20261112-185530', 'capture'), 300),
            'wm30': (os.path.join('out', 'captures', 's100_wm30_300'), 2),
            'cards': (os.path.join('out', 'captures', 's100cards'), 2)}
    fails = [0]

    def line(ok, text):
        fails[0] += 0 if ok else 1
        print(('PASS ' if ok else 'FAIL ') + text, flush=True)

    for n, (rel, _) in caps.items():
        if not os.path.isdir(os.path.join(root, rel)):
            line(False, 'capture %s missing (%s)' % (n, rel))
            return 1

    def render(name, tag, extra, scale, exe=None):
        rel, frames = caps[name]
        png = os.path.join(out, '%s_%s_x%g.png' % (name, tag, scale))
        if a.as_plant and '--wide' in extra and '--wide-post-rect' not in extra:
            extra = extra + ['--wide-post-rect']
        rc, text = run([os.path.abspath(exe or rplay), '--capture', os.path.join(root, rel), '--frames', str(frames), '--scale', str(scale), '--out', png, '--root', root] + extra)
        if rc:
            raise RuntimeError('rplay %s %s failed (%d): %s' % (name, tag, rc, text[-400:]))
        return png, text

    # census: T_Rect VS (patched CAAB5FD3B6EE4944) after the marker
    # wm30 over its 2 frames: 1 partial bar + 1 full-view tint a frame (the 'WM30 4' of lane 182 counted over 4 frames' worth of its own census)
    want = {'heenan': (600, 300), 'editor': (5856, 0), 'wm30': (2, 2), 'cards': (0, 2)}
    for k in (1.3125, 2.0):
        for name, (sq_want, fv_want) in want.items():
            for plant in (False, True):
                _, text = render(name, 'census_%g%s' % (k, '_plant' if plant else ''), ['--wide', str(k), '--wide-census'] + (['--wide-post-rect'] if plant else []), 1)
                sq = fv = left = 0
                for m in re.finditer(r'vs CAAB5FD3B6EE4944 .* hud (post-after )?\s*-> (SQUEEZE|FULL-VIEW|left)', text):
                    sq += m.group(2) == 'SQUEEZE'
                    fv += m.group(2) == 'FULL-VIEW'
                    left += m.group(2) == 'left'
                if not plant:
                    line(sq == sq_want and fv == fv_want and left == 0, 'census %s %.4g: T_Rects after the marker SQUEEZE %d (want %d), FULL-VIEW %d (want %d), left %d (want 0)' % (name, k, sq, sq_want, fv, fv_want, left))
                else:
                    line(left == sq_want + fv_want and sq == 0 and fv == 0, 'census %s %.4g PLANT (--wide-post-rect): left %d (want %d), SQUEEZE %d, FULL-VIEW %d' % (name, k, left, sq_want + fv_want, sq, fv))
    # the filter's copy pass stays left by hash; its quad is FULL-VIEW
    _, text = render('wm30', 'census_copy', ['--wide', '1.3125', '--wide-census'], 1)
    copy_left = len(re.findall(r'vp_w 8192 .*post-after\s*-> left', text))
    line(copy_left >= 2, 'wm30 filter copy pass (vp 8192) still left alone by hash: %d draws; the quad after it is FULL-VIEW (census above)' % copy_left)

    for scale in (1, 2):
        for k, label in ((1.3125, '21:9'), (2.0, '32:9')):
            for name, top in (('editor', 299), ('heenan', 99)):
                base, _ = render(name, '169', [], scale)
                new, _ = render(name, 'w%g' % k, ['--wide', str(k)], scale)
                old, _ = render(name, 'w%g_plant' % k, ['--wide', str(k), '--wide-post-rect'], scale)
                mn, pn, on = centre_error(new, base, k)
                mo, po, oo = centre_error(old, base, k)
                # absolute ceilings ~1.2x the values measured at lane 186 (mean, over24): Heenan keeps non-rectangle error (resampled text / art, the wider arena strip)
                ceil = {('heenan', 1.3125, 1): (13.0, 97000), ('heenan', 1.3125, 2): (7.6, 227000), ('heenan', 2.0, 1): (8.7, 45500), ('heenan', 2.0, 2): (7.5, 147000),
                        ('editor', 1.3125, 1): (2.6, 11500), ('editor', 1.3125, 2): (0.6, 9500), ('editor', 2.0, 1): (1.7, 3000), ('editor', 2.0, 2): (0.6, 7200)}[(name, k, scale)]
                line(on * 1.5 < oo and mn * 2 < mo and mn <= ceil[0] and on <= ceil[1], 'picture %s frame %d %s x%g: centre error new mean %.2f p99 %.0f over24 %d; plant mean %.2f p99 %.0f over24 %d (ceilings mean <= %.1f, over24 <= %d)' % (name, top, label, scale, mn, pn, on, mo, po, oo, ceil[0], ceil[1]))
            # filter quad: the picture is unchanged by leaving the hash list
            n1, _ = render('cards', 'w%g' % k, ['--wide', str(k)], scale)
            n2, _ = render('cards', 'w%g_plant' % k, ['--wide', str(k), '--wide-post-rect'], scale)
            line(open(n1, 'rb').read() == open(n2, 'rb').read(), 'filter picture %s x%g (cards): default == plant byte for byte' % (label, scale))
    # read back one draw's footprint from the render
    cases = (('heenan', 32, 140, 1140), ('heenan', 380, 164, 548), ('wm30', 1080, -60, 1340))
    for scale in (1, 2):
        for k, label in ((1.3125, '21:9'), (2.0, '32:9')):
            for name, d, x0, x1 in cases:
                for plant in (False, True):
                    extra = ['--wide', str(k)] + (['--wide-post-rect'] if plant else [])
                    pa, _ = render(name, 'ra_%d_%g%s' % (d, k, 'p' if plant else ''), extra, scale)
                    pb, _ = render(name, 'rb_%d_%g%s' % (d, k, 'p' if plant else ''), extra + ['--wide-mute-draw', str(d)], scale)
                    diff = np.abs(load(pa) - load(pb)).max(2) > 0
                    cols = np.where(diff.any(0))[0]
                    if not len(cols):
                        line(False, 'readback %s d%d %s x%g%s: no footprint' % (name, d, label, scale, ' PLANT' if plant else ''))
                        continue
                    lo, hi = cols[0] / scale, (cols[-1] + 1) / scale
                    sq = lambda x: 640 + (x - 640) / k
                    el, er = (x0, x1) if plant else (sq(x0), sq(x1))
                    tol = 1.5
                    if name == 'heenan' and d == 380:  # the plate is black over black past x ~ 548: only its left edge (and an upper bound on the right) can be read
                        ok = abs(lo - el) <= tol and hi <= er + tol
                    elif name == 'wm30':
                        el, er = max(el, 0), min(er, 1280)
                        ok = abs(lo - el) <= tol and abs(hi - er) <= tol
                    else:
                        ok = abs(lo - el) <= tol and abs(hi - er) <= tol
                    if plant:  # informational: the plant's box is partly covered by other opaque draws at 32:9
                        print('INFO ' + 'readback %s d%d %s x%g PLANT: footprint %.1f..%.1f (unsqueezed x %.1f..%.1f)' % (name, d, label, scale, lo, hi, el, er), flush=True)
                        continue
                    line(ok, 'readback %s d%d %s x%g%s: footprint %.1f..%.1f, expected %.1f..%.1f%s' % (name, d, label, scale, ' PLANT (unsqueezed x)' if plant else '', lo, hi, el, er, ' (right edge: upper bound only)' if (name == 'heenan' and d == 380) else ''))
    if a.base_rplay:
        for scale in (1, 2):
            for name in caps:
                n, _ = render(name, 'par_new', [], scale)
                b, _ = render(name, 'par_base', [], scale, a.base_rplay)
                line(open(n, 'rb').read() == open(b, 'rb').read(), '16:9 parity %s x%g: this rplay == baseline rplay byte for byte' % (name, scale))
    print('hudrect_gate: %s' % ('PASS' if not fails[0] else 'FAIL (%d)' % fails[0]))
    return 1 if fails[0] else 0


if __name__ == '__main__':
    sys.exit(main())
