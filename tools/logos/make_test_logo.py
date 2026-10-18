"""Our own test picture for the logo importer (Session 18, item 9). Nothing from the game.

256x256 RGBA: four coloured quarters, the word IMPORT, a frame, and a soft-edged transparent hole in the middle (so a
screenshot shows whether transparency survives).   py tools/logos/make_test_logo.py [OUT]  (default out/logos/test_logo.png)
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'out', 'logos', 'test_logo.png')
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
im = Image.new('RGBA', (256, 256))
d = ImageDraw.Draw(im)
for (x, y), c in zip(((0, 0), (128, 0), (0, 128), (128, 128)),
                     ((30, 200, 255, 255), (255, 210, 0, 255), (60, 220, 60, 255), (240, 60, 200, 255))):
    d.rectangle([x, y, x + 127, y + 127], fill=c)
d.rectangle([2, 2, 253, 253], outline=(255, 255, 255, 255), width=4)
try:
    f = ImageFont.truetype('arialbd.ttf', 44)
except OSError:
    f = ImageFont.load_default()
d.text((128, 40), 'IMPORT', font=f, fill=(0, 0, 0, 255), anchor='mm')
px = im.load()
for y in range(256):
    for x in range(256):
        r = math.hypot(x - 128, y - 150)
        if r < 50:
            a = 0 if r < 35 else int(255 * (r - 35) / 15)
            p = px[x, y]
            px[x, y] = (p[0], p[1], p[2], min(p[3], a))
im.save(out)
print(out)
