#!/usr/bin/env python3
"""S102 lane R18-B2: the before / after / difference x8 sheet for level 2 (the flat-pixel FXAA exit), from the PNGs the check's fview runs dumped (out\\r18\\d_<cap>_<arm>\\frame_NNNNN.png).
  py -I tools\\r18\\r18_b2_sheet.py [--root DIR] [--out DIR]
Rows: three Bryan frames and one match frame. Columns: level 0 (off), level 2, |difference| x 8, and a 4x zoom crop of the 200x120 window with the most changed pixels (nearest-neighbour).
Prints, per frame, the pixels over 8 / 255 and the max difference (owned_check's tolerance reading) and writes sheet.png + stats.txt + KEEP (reason + until)."""
import argparse
import datetime
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def main_checkout(root):
    import subprocess
    try:
        c = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        return os.path.dirname(c) if c else root
    except Exception:
        return root


def load(p):
    if not os.path.isfile(p):
        raise SystemExit('FAIL cannot read %s' % p)
    return np.asarray(Image.open(p).convert('RGB')).astype(int)


def best_window(d, w=200, h=120):
    m = (d > 0).astype(np.int32)
    ii = m.cumsum(0).cumsum(1)
    best, at = -1, (0, 0)
    for y in range(0, d.shape[0] - h, 20):
        for x in range(0, d.shape[1] - w, 20):
            s = ii[y + h - 1, x + w - 1] - (ii[y - 1, x + w - 1] if y else 0) - (ii[y + h - 1, x - 1] if x else 0) + (ii[y - 1, x - 1] if x and y else 0)
            if s > best:
                best, at = s, (x, y)
    return at


def make_clip(work, out, first=90, count=60, fps=6):
    """A short slowed (6 fps; the game runs at 30) side-by-side clip of 60 consecutive Bryan frames: level 0 | level 2 | difference x 8, and the 4x crop of the most changed window below.
    Every panel is labelled. Needs ffmpeg on PATH (or the winget copy)."""
    import glob
    import shutil
    import subprocess
    ff = shutil.which('ffmpeg') or next(iter(glob.glob(os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Microsoft', 'WinGet', 'Packages', 'Gyan.FFmpeg*', '*', 'bin', 'ffmpeg.exe'))), None)
    if not ff:
        raise SystemExit('FAIL no ffmpeg for the clip')
    frames = os.path.join(work, 'clip_frames')
    shutil.rmtree(frames, ignore_errors=True)
    os.makedirs(frames)
    offs = [load(os.path.join(work, 'd_bryan_off', 'frame_%05d.png' % f)) for f in range(first, first + count)]
    l2s = [load(os.path.join(work, 'd_bryan_l2', 'frame_%05d.png' % f)) for f in range(first, first + count)]
    total = sum(np.abs(a - b).max(axis=2) > 0 for a, b in zip(offs, l2s))
    x, y = best_window(total)
    labels = ['level 0 (off: the game\'s own smoothing)', 'level 2 (flat exit)', 'difference x 8']
    for n, (a, b) in enumerate(zip(offs, l2s)):
        d = np.clip(np.abs(a - b) * 8, 0, 255).astype(np.uint8)
        top = [Image.fromarray(im.astype(np.uint8)).resize((640, 360), Image.LANCZOS) for im in (a, b, d)]
        bot = [Image.fromarray(im.astype(np.uint8)[y:y + 120, x:x + 200]).resize((640, 384), Image.NEAREST) for im in (a, b, d)]
        sheet = Image.new('RGB', (1920, 24 + 360 + 24 + 384), (20, 20, 20))
        dr = ImageDraw.Draw(sheet)
        for c in range(3):
            dr.text((c * 640 + 6, 6), '%s   Bryan frame %d  (playback 6 fps, game 30 fps)' % (labels[c], first + n), fill=(255, 255, 255))
            sheet.paste(top[c], (c * 640, 24))
            dr.text((c * 640 + 6, 24 + 360 + 6), 'crop 4x of x %d y %d: %s' % (x, y, labels[c]), fill=(255, 255, 255))
            sheet.paste(bot[c], (c * 640, 24 + 360 + 24))
        sheet.save(os.path.join(frames, 'f_%04d.png' % n))
    clip = os.path.join(out, 'r18_b2_level2_clip.mp4')
    r = subprocess.run([ff, '-y', '-framerate', str(fps), '-i', os.path.join(frames, 'f_%04d.png'), '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '14', clip], capture_output=True, text=True)
    shutil.rmtree(frames, ignore_errors=True)
    if r.returncode != 0 or not os.path.isfile(clip):
        raise SystemExit('FAIL ffmpeg: ' + r.stderr[-400:])
    print('wrote', clip)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--out', default='')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    work = os.path.join(root, 'out', 'r18')
    out = a.out or os.path.join(main_checkout(root), 'out', 's102', 'sheets', 'r18')
    os.makedirs(out, exist_ok=True)
    rows = [('bryan', '00030'), ('bryan', '00100'), ('bryan', '00200'), ('match', '00100')]
    tiles, stats = [], []
    for cap, f in rows:
        off = load(os.path.join(work, 'd_%s_off%s' % (cap, '' if cap == 'bryan' else '_d'), 'frame_%s.png' % f)) if cap == 'bryan' else load(os.path.join(work, 'd_%s_off_d' % cap, 'frame_%s.png' % f))
        l2 = load(os.path.join(work, 'd_%s_%s' % (cap, 'l2' if cap == 'bryan' else 'l2_d'), 'frame_%s.png' % f))
        d = np.abs(off - l2).max(axis=2)
        x, y = best_window(d)
        crop = lambda im: Image.fromarray(im[y:y + 120, x:x + 200].astype(np.uint8)).resize((800, 480), Image.NEAREST)
        diff8 = np.clip(np.abs(off - l2) * 8, 0, 255).astype(np.uint8)
        full = lambda im: Image.fromarray(im.astype(np.uint8)).resize((640, 360), Image.LANCZOS)
        stats.append('%s frame %s: differing pixels %d, over 8/255 %d, max difference %d of 255, of %d (crop window x %d y %d)' % (cap, f, (d > 0).sum(), (d > 8).sum(), d.max(), d.size, x, y))
        tiles.append([full(off), full(l2), full(diff8), crop(off), crop(l2), Image.fromarray(diff8[y:y + 120, x:x + 200]).resize((800, 480), Image.NEAREST)])
    cw = [640, 640, 640, 800, 800, 800]
    sheet = Image.new('RGB', (sum(cw), 380 * 2 * 0 + (360 + 480 + 24) * len(tiles)), (20, 20, 20))
    dr = ImageDraw.Draw(sheet)
    y = 0
    for r, row in enumerate(tiles):
        x = 0
        for c, im in enumerate(row):
            sheet.paste(im, (x, y + 24))
            dr.text((x + 6, y + 6), ['level 0 (off)', 'level 2 (flat exit)', 'difference x 8', 'crop level 0 (4x)', 'crop level 2 (4x)', 'crop difference x 8 (4x)'][c] + '  ' + stats[r].split(':')[0], fill=(255, 255, 255))
            x += cw[c]
        y += 360 + 480 + 24
    sheet.save(os.path.join(out, 'r18_b2_level2_sheet.png'))
    with open(os.path.join(out, 'stats.txt'), 'w') as f:
        f.write('\n'.join(stats) + '\n')
    until = (datetime.date.today() + datetime.timedelta(days=14)).isoformat()
    with open(os.path.join(out, 'KEEP'), 'w') as f:
        f.write('reason: R18-B2 level 2 (flat-pixel FXAA exit) before / after sheet for Jarrod to judge\nuntil %s\n' % until)
    print('\n'.join(stats))
    print('wrote', os.path.join(out, 'r18_b2_level2_sheet.png'))
    make_clip(work, out)


if __name__ == '__main__':
    sys.exit(main())
