#!/usr/bin/env python3
r"""One picture sheet of a replayed frame's surfaces, in the order the frame first used them (rplay --gallery DIR). Stays under out\ (rule 1).

  py tools\renderer\gallery_sheet.py DIR [--columns 7]
"""
import argparse
import glob
import os

from PIL import Image, ImageDraw

CELL_WIDTH, CELL_HEIGHT, LABEL = 270, 152, 16


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--columns', type=int, default=7)
    args = ap.parse_args()
    files = [f for f in sorted(glob.glob(os.path.join(args.folder, '*.png'))) if os.path.basename(f) != 'sheet.png']
    rows = (len(files) + args.columns - 1) // args.columns
    sheet = Image.new('RGB', (args.columns * CELL_WIDTH, rows * (CELL_HEIGHT + LABEL)), (28, 28, 34))
    draw = ImageDraw.Draw(sheet)
    for index, path in enumerate(files):
        x, y = (index % args.columns) * CELL_WIDTH, (index // args.columns) * (CELL_HEIGHT + LABEL)
        picture = Image.open(path).convert('RGB')
        picture.thumbnail((CELL_WIDTH - 4, CELL_HEIGHT - 2))
        sheet.paste(picture, (x + 2, y + LABEL))
        draw.text((x + 3, y + 2), os.path.basename(path)[:-4], fill=(230, 230, 120))
    out = os.path.join(args.folder, 'sheet.png')
    sheet.save(out)
    print('%d surfaces, %s' % (len(files), out))


if __name__ == '__main__':
    main()
