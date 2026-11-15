r"""S102 lane L1: pictures of the analytic pre-light against the cube path, 0 game runs (fview headless).
  py -I tools\l1\l1_pics.py [--root DIR] [--cap bryan|brock] [--frames 20,60,85] [--out DIR] [--sheet] [--bundle]
Per frame: the presented picture (surface 1) of the stream's cube path (OFF) and of the analytic path (--prelight-analytic, ON), and the WRESTLER mask: the pixels a `--draw-drop` of the wrestlers' draws
changes (the cube only ever lights wrestlers, so every difference must lie inside it). Prints, per frame and in total, over the wrestler pixels: the mean absolute difference (0..255 per channel), the
99th / 99.9th percentile, the maximum, and the share of pixels differing by more than 0 / 2 / 4 / 8 / 12 levels; and the number of differing pixels OUTSIDE the mask (must be 0).
With --sheet it writes wrestler crops (off | on | difference x 8) under --out (the lane's sheet). Library use: render(), diff_stats()."""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
BUNDLE = ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours']


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def render_arm(fview, cap, frames, out_dir, extra, timeout=1800):
    """One fview replay of the capture up to the last wanted frame with the chosen frames dumped as PNG: {frame: int16 HxWx3}."""
    os.makedirs(out_dir, exist_ok=True)
    for f in os.listdir(out_dir):
        if f.startswith('frame_'):
            os.remove(os.path.join(out_dir, f))
    cmd = [fview, '--capture', cap, '--validation', '1', '--frames', str(max(frames) + 1), '--small-passes-ours', '--dump-frames', ','.join(str(x) for x in frames), '--dump-dir', out_dir] + list(extra)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
    text = (p.stdout or '') + (p.stderr or '')
    got = {}
    for fr in frames:
        path = os.path.join(out_dir, 'frame_%05d.png' % fr)
        if not os.path.exists(path):
            raise RuntimeError('fview wrote no picture for frame %d (%s, rc %d): %s' % (fr, extra, p.returncode, text[-300:]))
        got[fr] = np.array(Image.open(path).convert('RGB')).astype(np.int16)
    return got, text


def diff_stats(a, b, mask):
    d = np.abs(a - b).max(axis=2)
    inside = d[mask]
    n = max(1, inside.size)
    out = dict(pixels=int(inside.size), mean=float(np.abs(a - b)[mask].mean()) if inside.size else 0.0, p99=float(np.percentile(inside, 99)) if inside.size else 0.0,
               p999=float(np.percentile(inside, 99.9)) if inside.size else 0.0, max=int(inside.max()) if inside.size else 0, outside=int((d[~mask] > 0).sum()))
    for t in (0, 2, 4, 8, 12):
        out['gt%d' % t] = float((inside > t).sum()) / n
    return out


def crop_box(mask, pad=24):
    ys, xs = np.nonzero(mask)
    if not ys.size:
        return 0, 0, mask.shape[1], mask.shape[0]
    return max(0, xs.min() - pad), max(0, ys.min() - pad), min(mask.shape[1], xs.max() + pad), min(mask.shape[0], ys.max() + pad)


def close_box(off, on, mask, size=128):
    """the size x size window of the wrestler pixels where the two pictures differ most (the skin highlight / SSS tint close shot)"""
    d = (np.abs(off - on).max(axis=2) * mask).astype(np.float64)
    ii = np.pad(d, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    h, w = d.shape
    best, at = -1.0, (0, 0)
    for y in range(0, h - size, 8):
        row = ii[y + size, size:] - ii[y, size:] - ii[y + size, :w - size + 1] + ii[y, :w - size + 1]
        x = int(np.argmax(row))
        if row[x] > best:
            best, at = row[x], (x, y)
    return at[0], at[1], at[0] + size, at[1] + size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--cap', default='bryan')
    ap.add_argument('--frames', default='20,60,85')
    ap.add_argument('--out', default='')
    ap.add_argument('--sheet', action='store_true')
    ap.add_argument('--bundle', action='store_true')
    ap.add_argument('--zoom', type=int, default=2)
    ap.add_argument('--extra', default='', help='more fview flags for the ON arm, space separated (e.g. "--prelight-analytic-fault quantise")')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_dir = main_checkout(root)
    fview = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    cap = os.path.join(main_dir, 'out', 'lane86', a.cap, 'capture')
    work = os.path.join(root, 'out', 'l1', 'pics_' + a.cap)
    os.makedirs(work, exist_ok=True)
    base = (BUNDLE if a.bundle else []) + ['--copy-batch', 'on']
    frames = [int(x) for x in a.frames.split(',')]
    tiles = []
    closes = []
    tot = dict(pixels=0, sum=0.0, mx=0, outside=0)
    offs, _ = render_arm(fview, cap, frames, os.path.join(work, 'off'), base)
    ons, on_text = render_arm(fview, cap, frames, os.path.join(work, 'on'), base + ['--prelight-analytic'] + a.extra.split())
    drops, _ = render_arm(fview, cap, frames, os.path.join(work, 'drop'), base + ['--draw-drop', '@class=wrestlers'])
    print([l for l in on_text.splitlines() if 'prelight_analytic' in l][-1:])
    for fr in frames:
        off, on, nowr = offs[fr], ons[fr], drops[fr]
        mask = np.abs(off - nowr).max(axis=2) > 0
        s = diff_stats(off, on, mask)
        print('frame %d: wrestler pixels %d, mean %.3f, p99 %.0f, p99.9 %.0f, max %d, >0 %.1f%% >2 %.1f%% >4 %.1f%% >8 %.2f%% >12 %.3f%%, differing OUTSIDE the wrestlers %d' % (
            fr, s['pixels'], s['mean'], s['p99'], s['p999'], s['max'], 100 * s['gt0'], 100 * s['gt2'], 100 * s['gt4'], 100 * s['gt8'], 100 * s['gt12'], s['outside']))
        tot['pixels'] += s['pixels']
        tot['sum'] += s['mean'] * s['pixels']
        tot['mx'] = max(tot['mx'], s['max'])
        tot['outside'] += s['outside']
        if a.sheet:
            cx0, cy0, cx1, cy1 = close_box(off, on, mask)
            closes.append((fr, [Image.fromarray(im[cy0:cy1, cx0:cx1].astype(np.uint8)) for im in (off, on)], Image.fromarray(np.clip(np.abs(off - on)[cy0:cy1, cx0:cx1] * 8, 0, 255).astype(np.uint8)), (cx0, cy0)))
            x0, y0, x1, y1 = crop_box(mask)
            tiles.append((fr, [Image.fromarray(im[y0:y1, x0:x1].astype(np.uint8)) for im in (off, on)], Image.fromarray(np.clip(np.abs(off - on)[y0:y1, x0:x1] * 8, 0, 255).astype(np.uint8))))
    print('TOTAL wrestler pixels %d, mean %.3f, max %d, outside %d' % (tot['pixels'], tot['sum'] / max(1, tot['pixels']), tot['mx'], tot['outside']))
    if a.sheet and tiles:
        z = a.zoom if max(t[1][0].width for t in tiles) <= 600 else 1
        w = max(t[1][0].width for t in tiles) * z
        h = sum(t[1][0].height * z + 18 for t in tiles)
        sheet = Image.new('RGB', (3 * w + 40, h + 24), (24, 24, 24))
        d = ImageDraw.Draw(sheet)
        for i, title in enumerate(('OFF: the baked cube', 'ON: four lights per pixel', 'difference x 8')):
            d.text((10 + i * (w + 10), 4), title, fill=(230, 230, 230))
        y = 20
        for fr, (off, on), diff in tiles:
            for i, im in enumerate((off, on, diff)):
                sheet.paste(im.resize((im.width * z, im.height * z), Image.NEAREST), (10 + i * (w + 10), y + 14))
            d.text((10, y), '%s frame %d' % (a.cap, fr), fill=(255, 220, 120))
            y += off.height * z + 18
        os.makedirs(a.out, exist_ok=True)
        path = os.path.join(a.out, 'l1_prelight_%s%s.png' % (a.cap, '_bundle' if a.bundle else ''))
        sheet.save(path)
        print('sheet: %s' % path)
        z = 4
        cw = closes[0][1][0].width * z
        ch = closes[0][1][0].height * z
        close = Image.new('RGB', (3 * cw + 40, len(closes) * (ch + 18) + 24), (24, 24, 24))
        d = ImageDraw.Draw(close)
        for i, title in enumerate(('OFF: the baked cube', 'ON: four lights per pixel', 'difference x 8')):
            d.text((10 + i * (cw + 10), 4), title, fill=(230, 230, 230))
        y = 20
        for fr, (off, on), diff, (cx, cy) in closes:
            for i, im in enumerate((off, on, diff)):
                close.paste(im.resize((cw, ch), Image.NEAREST), (10 + i * (cw + 10), y + 14))
            d.text((10, y), '%s frame %d, the window (%d,%d) %dx%d where the two differ most, 4x' % (a.cap, fr, cx, cy, off.width, off.height), fill=(255, 220, 120))
            y += ch + 18
        cpath = os.path.join(a.out, 'l1_prelight_%s%s_close.png' % (a.cap, '_bundle' if a.bundle else ''))
        close.save(cpath)
        print('sheet: %s' % cpath)
    return 1 if tot['outside'] else 0


if __name__ == '__main__':
    sys.exit(main())
