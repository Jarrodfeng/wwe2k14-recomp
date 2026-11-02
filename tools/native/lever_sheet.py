"""Session 82 (overnight, the look levers): a picture sheet for one lever - the same shots with the lever off | on | the
difference (x8), one row per shot, a line of numbers under each row (the owned_verdict tolerance numbers: p99.9, worst
16x16 block mean, share of pixels over 4, max). For Jarrod's eyes; no lever becomes a default.

    py -I tools\\native\\lever_sheet.py OUT.jpg LABEL OFF_DIR ON_DIR NAME [NAME ...]

OFF_DIR / ON_DIR hold the shots (captures\\<run>_<NAME>.png); a directory argument may be DIR@RUN to take the shots of run
RUN only (e.g. out\\harness\\bisect\\exe\\sdk_x\\captures@003).
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw


def find(dirspec, name):
    d, _, run = dirspec.partition('@')
    for f in sorted(os.listdir(d)):
        if f.endswith(f'_{name}.png') and (not run or f.startswith(run + '_')):
            return os.path.join(d, f)
    raise SystemExit(f'no shot {name} in {dirspec}')


def stats(a, b):
    n = np.abs(a.astype(int) - b.astype(int)).max(axis=2)
    h, w = n.shape
    bh, bw = h // 16, w // 16
    blocks = n[:bh * 16, :bw * 16].reshape(bh, 16, bw, 16).mean(axis=(1, 3))
    return float(np.percentile(n, 99.9)), float(blocks.max()), float((n > 4).mean()) * 100, int(n.max()), n


def main():
    out, label, off_dir, on_dir = sys.argv[1:5]
    names = sys.argv[5:]
    rows = []
    for name in names:
        a = np.asarray(Image.open(find(off_dir, name)).convert('RGB'))
        b = np.asarray(Image.open(find(on_dir, name)).convert('RGB'))
        p999, block, share, mx, n = stats(a, b)
        heat = np.repeat(np.clip(n * 8, 0, 255)[..., None], 3, axis=2).astype(np.uint8)
        scale = 640 / a.shape[1]
        size = (640, int(a.shape[0] * scale))
        panels = [Image.fromarray(x).resize(size, Image.LANCZOS) for x in (a, b, heat)]
        row = Image.new('RGB', (size[0] * 3, size[1] + 22), (20, 20, 20))
        for i, p in enumerate(panels):
            row.paste(p, (i * size[0], 0))
        d = ImageDraw.Draw(row)
        d.text((4, size[1] + 5), f'{label}: {name}   off | on | difference x8   p99.9 {p999:.1f}  worst block mean {block:.2f}  '
                                 f'share>4 {share:.3f}%  max {mx}', fill=(230, 230, 230))
        rows.append(row)
        print(f'{name}: p99.9 {p999:.1f} block {block:.2f} share>4 {share:.3f}% max {mx}')
    sheet = Image.new('RGB', (rows[0].width, sum(r.height for r in rows)))
    y = 0
    for r in rows:
        sheet.paste(r, (0, y))
        y += r.height
    sheet.save(out, quality=88)
    print('sheet', out)


if __name__ == '__main__':
    main()
