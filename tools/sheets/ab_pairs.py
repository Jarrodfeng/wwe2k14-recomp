r"""A / B pair sheet (S105): rows of [A | B | where they differ], each panel labelled, for a before / after check Jarrod reads by eye.

  py -I tools\sheets\ab_pairs.py --out out\s105\b1\sheet.jpg --a-label "cull ON" --b-label "cull OFF" A1.png B1.png [A2.png B2.png ...]

The third panel is B's picture greyed with the pixels whose max channel difference is over --thresh (default 24) painted red, plus the
count of such pixels, so a change too small to see side by side is still pointed at. Pictures are scaled to --width per panel. Prints one
line per pair (changed pixel share). Game screenshots: local only, never committed (CLAUDE.md rule 1).
"""
import argparse
import os
import sys

from PIL import Image, ImageChops, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--a-label', default='A')
    ap.add_argument('--b-label', default='B')
    ap.add_argument('--width', type=int, default=640)
    ap.add_argument('--thresh', type=int, default=24)
    ap.add_argument('pics', nargs='+')
    a = ap.parse_args()
    if len(a.pics) % 2:
        sys.exit('ab_pairs: give pictures in A B pairs')
    rows = []
    for i in range(0, len(a.pics), 2):
        pa, pb = Image.open(a.pics[i]).convert('RGB'), Image.open(a.pics[i + 1]).convert('RGB')
        if pb.size != pa.size:
            pb = pb.resize(pa.size)
        diff = ImageChops.difference(pa, pb)
        mask = diff.convert('RGB').point(lambda v: 255 if v > a.thresh else 0).convert('L')
        # a pixel counts when any channel is over the threshold
        r, g, b = diff.split()
        m = ImageChops.lighter(ImageChops.lighter(r, g), b).point(lambda v: 255 if v > a.thresh else 0)
        n = sum(1 for v in m.getdata() if v)
        share = n / float(pa.size[0] * pa.size[1])
        grey = pb.convert('L').convert('RGB')
        red = Image.new('RGB', pa.size, (255, 0, 0))
        heat = Image.composite(red, grey, m)
        h = int(pa.size[1] * a.width / pa.size[0])
        name = os.path.basename(a.pics[i])
        panels = [(pa, '%s  %s' % (a.a_label, name)), (pb, '%s  %s' % (a.b_label, os.path.basename(a.pics[i + 1]))),
                  (heat, 'differs (> %d): %d px, %.3f %%' % (a.thresh, n, 100 * share))]
        row = Image.new('RGB', (a.width * 3, h + 22), (0, 0, 0))
        for k, (img, label) in enumerate(panels):
            row.paste(img.resize((a.width, h)), (k * a.width, 22))
            ImageDraw.Draw(row).text((k * a.width + 4, 4), label, fill=(255, 255, 0))
        rows.append(row)
        print('%s vs %s: %d px differ (%.3f %%)' % (a.pics[i], a.pics[i + 1], n, 100 * share))
    sheet = Image.new('RGB', (rows[0].size[0], sum(r.size[1] for r in rows)))
    y = 0
    for r in rows:
        sheet.paste(r, (0, y))
        y += r.size[1]
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    sheet.save(a.out, quality=90)
    print('sheet:', a.out)


if __name__ == '__main__':
    main()
