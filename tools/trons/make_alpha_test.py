"""Our own transparent test reveal (Session 18, item 2). Nothing from the game.

880x720 (the size of the game's title reveals): colour bars and a frame counter inside a circle with a soft edge
(fully solid in the middle, fading to fully clear over ~60 px), everything outside it fully transparent. Writes a PNG
sequence (RGBA) to out/trons/alpha_test/ and turns it into a stacked-alpha MP4 with make_stacked_alpha.py.

  py tools/trons/make_alpha_test.py [--out out/trons/alpha_test.alpha.mp4] [--seconds 6]
"""
import argparse
import math
import os
import subprocess
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(__file__))
from make_test_pattern import ROOT, font  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'trons', 'alpha_test.alpha.mp4'))
    ap.add_argument('--seconds', type=float, default=6.0)
    a = ap.parse_args()
    w, h, n = 880, 720, int(round(a.seconds * 30000 / 1001))
    frames = os.path.join(ROOT, 'out', 'trons', 'alpha_test')
    os.makedirs(frames, exist_ok=True)
    bars = [(235, 235, 235), (235, 235, 16), (16, 235, 235), (16, 235, 16), (235, 16, 235), (235, 16, 16),
            (16, 16, 235), (16, 16, 16)]
    colour = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(colour)
    for i, c in enumerate(bars):
        d.rectangle([i * w // 8, 0, (i + 1) * w // 8 - 1, h - 1], fill=c)
    # Alpha: solid inside radius r0, a smooth fade to clear at r1.
    cx, cy, r0, r1 = w / 2, h / 2, 250.0, 310.0
    alpha = Image.new('L', (w, h))
    px = alpha.load()
    for y in range(h):
        for x in range(w):
            r = math.hypot(x - cx, y - cy)
            t = min(1.0, max(0.0, (r1 - r) / (r1 - r0)))
            px[x, y] = int(round(255 * (t * t * (3 - 2 * t))))
    big = font(120)
    for f in range(n):
        im = colour.copy()
        dd = ImageDraw.Draw(im)
        dd.rectangle([cx - 170, cy - 80, cx + 170, cy + 80], fill=(0, 0, 0))
        dd.text((cx, cy), f'{f:03d}', font=big, fill=(255, 255, 255), anchor='mm')
        im.putalpha(alpha)
        im.save(os.path.join(frames, f'{f:04d}.png'))
    tool = os.path.join(os.path.dirname(__file__), 'make_stacked_alpha.py')
    r = subprocess.run([sys.executable, tool, os.path.join(frames, '%04d.png'), a.out, '--fps', '30000/1001'])
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()
