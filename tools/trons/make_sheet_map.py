"""Our own 320x320 "sheet map" video for mapping the tron sheet (Session 18, item 1). Nothing from the game.

A character tron is a sheet (Session 17/18, measured on 6 trons): rows 0-223 = the big screen, then four 320x24
strips at rows 224, 248, 272, 296 that feed the banner screens circling the arena. This video fills each region with
something you can recognise in a screenshot:
  big screen: a grid, "BIG" and the four corners marked TL / TR / BL / BR;
  strips S1-S4: each its own colour (red, green, blue, yellow), its number and a column ruler (0-320 every 40).
A small white block moves left to right along every strip once per loop, so two screenshots tell whether the game
scrolls a strip itself.

  py tools/trons/make_sheet_map.py [--out out/trons/sheet_map.mp4] [--seconds 6] [--scale 1]

--scale 2 makes a 640x640 copy (same layout, twice the pixels).
--banner makes a 1280x48 banner video instead (for NNN.banner.mp4): four segments B1-B4 (orange, cyan, purple, white)
and a block that runs the whole width once per loop; wider than 26.7:1, so the shim spreads it over the four strips
(the ring shows them end to end). Output goes in out/ (gitignored).
"""
import argparse
import os
import subprocess
import sys
from fractions import Fraction

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(__file__))
from make_test_pattern import ROOT, find_ffmpeg, font  # noqa: E402

STRIPS = [(224, (220, 30, 30)), (248, (30, 200, 30)), (272, (40, 80, 230)), (296, (230, 210, 30))]
STRIP_H = 24


def base_image():
    im = Image.new('RGB', (320, 320), (20, 20, 20))
    d = ImageDraw.Draw(im)
    for x in range(0, 321, 40):  # big screen grid
        d.line([x, 0, x, 223], fill=(90, 90, 90))
    for y in range(0, 224, 28):
        d.line([0, y, 319, y], fill=(90, 90, 90))
    d.rectangle([0, 0, 319, 223], outline=(255, 255, 255), width=2)
    f_big, f_mid, f_small = font(64), font(18), font(11)
    d.text((160, 112), 'BIG', font=f_big, fill=(255, 255, 255), anchor='mm')
    for text, xy, anchor in (('TL', (4, 3), 'la'), ('TR', (316, 3), 'ra'), ('BL', (4, 220), 'ld'),
                             ('BR', (316, 220), 'rd')):
        d.rectangle([xy[0] - 3 if anchor[0] == 'l' else xy[0] - 30, xy[1] - 2 if anchor[1] == 'a' else xy[1] - 22,
                     xy[0] + 30 if anchor[0] == 'l' else xy[0] + 3, xy[1] + 22 if anchor[1] == 'a' else xy[1] + 2],
                    fill=(255, 0, 255))
        d.text(xy, text, font=f_mid, fill=(255, 255, 255), anchor=anchor)
    for i, (y0, col) in enumerate(STRIPS):
        d.rectangle([0, y0, 319, y0 + STRIP_H - 1], fill=col)
        for x in range(0, 320, 40):  # ruler
            d.line([x, y0, x, y0 + 5], fill=(0, 0, 0), width=2)
            d.text((x + 2, y0 + STRIP_H - 1), str(x), font=f_small, fill=(0, 0, 0), anchor='ld')
        for x in (20, 180):
            d.text((x + 12, y0 + 12), f'S{i + 1}', font=f_mid, fill=(255, 255, 255), anchor='lm')
        d.text((312, y0 + 12), '>', font=f_mid, fill=(255, 255, 255), anchor='rm')
    return im


def banner(a):
    fps = Fraction(a.fps)
    n = int(round(a.seconds * fps))
    w, h = 1280, 48
    if a.out.endswith('sheet_map.mp4'):
        a.out = a.out.replace('sheet_map.mp4', 'banner_map.mp4')
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    cols = [(240, 140, 20), (20, 200, 220), (140, 50, 200), (230, 230, 230)]
    base = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(base)
    for i, c in enumerate(cols):
        d.rectangle([i * 320, 0, i * 320 + 319, h - 1], fill=c)
        for x in (40, 200):
            d.text((i * 320 + x, h // 2), f'B{i + 1}', font=font(30), fill=(0, 0, 0), anchor='lm')
    cmd = [find_ffmpeg(), '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{w}x{h}',
           '-r', f'{fps.numerator}/{fps.denominator}', '-i', '-', '-c:v', 'libx264', '-preset', 'medium', '-crf', '14',
           '-pix_fmt', 'yuv420p', '-movflags', '+faststart', a.out]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for f in range(n):
        im = base.copy()
        x = f * (w - 24) // n
        ImageDraw.Draw(im).rectangle([x, 10, x + 24, h - 11], fill=(255, 0, 0))
        p.stdin.write(im.tobytes())
    p.stdin.close()
    if p.wait():
        sys.exit(f'ffmpeg failed ({p.returncode})')
    print(f'{a.out}: {w}x{h} banner, {n} frames at {fps} fps')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'trons', 'sheet_map.mp4'))
    ap.add_argument('--seconds', type=float, default=6.0)
    ap.add_argument('--fps', default='30000/1001')
    ap.add_argument('--scale', type=int, default=1)
    ap.add_argument('--banner', action='store_true')
    a = ap.parse_args()
    if a.banner:
        return banner(a)
    fps = Fraction(a.fps)
    n = int(round(a.seconds * fps))
    s = 320 * a.scale
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    base = base_image()
    cmd = [find_ffmpeg(), '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{s}x{s}',
           '-r', f'{fps.numerator}/{fps.denominator}', '-i', '-', '-c:v', 'libx264', '-preset', 'medium', '-crf', '14',
           '-pix_fmt', 'yuv420p', '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709',
           '-color_range', 'tv', '-movflags', '+faststart', a.out]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for f in range(n):
        im = base.copy()
        d = ImageDraw.Draw(im)
        x = f * 300 // n
        for y0, _ in STRIPS:
            d.rectangle([x, y0 + 8, x + 12, y0 + 16], fill=(255, 255, 255))
        d.text((160, 170), f'{f:03d}', font=font(28), fill=(255, 255, 0), anchor='mm')
        if a.scale != 1:
            im = im.resize((s, s), Image.NEAREST)
        p.stdin.write(im.tobytes())
    p.stdin.close()
    if p.wait():
        sys.exit(f'ffmpeg failed ({p.returncode})')
    print(f'{a.out}: {s}x{s}, {n} frames at {fps} fps')


if __name__ == '__main__':
    main()
