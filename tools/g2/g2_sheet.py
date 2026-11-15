#!/usr/bin/env python3
"""S102 G2: the sheet for Jarrod: SGSR vs plain bilinear vs the 1280-tall cap, Bryan's entrance at the 2736x1824 panel size (shots from tools\\g2\\g2_check.py --bryan).
  py -I tools\\g2\\g2_sheet.py [--root DIR] [--out DIR]
Rows: the whole picture (scaled to fit), then a 1:1 crop of 700x440 from the middle (where the wrestler is) and one of the lower left (crowd / ring edge). Every panel is labelled."""
import argparse
import os
import sys

from PIL import Image, ImageDraw

ap = argparse.ArgumentParser()
ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
ap.add_argument('--out', default=None)
a = ap.parse_args()
work = os.path.join(a.root, 'out', 's102', 'g2', 'check')
out = a.out or os.path.join(a.root, 'out', 's102', 'sheets', 'g2')
os.makedirs(out, exist_ok=True)
arms = [('SGSR (LOW today)', 'bryan_sgsr.png'), ('bilinear (lever c)', 'bryan_bilinear.png'), ('cap 1920x1280 + bilinear blit (lever b)', 'bryan_cap.png')]
imgs = []
for label, name in arms:
    p = os.path.join(work, name)
    if not os.path.isfile(p):
        sys.exit('missing ' + p + ' (run tools\\g2\\g2_check.py --bryan first)')
    imgs.append((label, Image.open(p).convert('RGB')))
W, H = imgs[0][1].size
cw, ch = 700, 440
crops = [(W // 2 - cw // 2, H // 2 - ch // 2), (int(W * 0.12), int(H * 0.62))]
pw = 700
rows = [[im.resize((pw, int(H * pw / W)), Image.LANCZOS) for _, im in imgs]] + [[im.crop((x, y, x + cw, y + ch)) for _, im in imgs] for x, y in crops]
title = ['whole picture, scaled to fit', 'crop 1:1, middle (%d,%d)' % crops[0], 'crop 1:1, lower left (%d,%d)' % crops[1]]
lab = 22
sheet = Image.new('RGB', (pw * 3 + 40, sum(r[0].size[1] + lab * 2 for r in rows) + 10), (24, 24, 24))
d = ImageDraw.Draw(sheet)
y = 5
for r, t in zip(rows, title):
    d.text((10, y), t, fill=(255, 255, 255))
    y += lab
    for i, (im, (label, _)) in enumerate(zip(r, arms)):
        sheet.paste(im, (10 + i * (pw + 10), y + lab // 1))
        d.text((10 + i * (pw + 10), y), label, fill=(255, 220, 120))
    y += r[0].size[1] + lab
sheet.save(os.path.join(out, 'g2_output_pass_sheet.png'))
print('wrote', os.path.join(out, 'g2_output_pass_sheet.png'), sheet.size)
