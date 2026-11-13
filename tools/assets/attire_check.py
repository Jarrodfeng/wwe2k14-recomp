"""Session 100: checks the attires our renderer packed in game against the attires built from the files.

    py -I tools\\assets\\attire_check.py [--packs out\\captures\\live] [--refs out\\s100\\attires] [--sheet out\\s100\\attire_check.png]

--packs: the pack_<frame>_<texture>.png pictures a run wrote with --wwe2k14_rdr_debug_pack_png=true (owner mode: our renderer's own atlas,
level 0, as packed into the wrestler's texture). --refs: tools\\assets\\attire_build.py's attire_<id>_<attire>.png. Each packed atlas is
compared (at 256x128, mean absolute difference over RGB, 0-255) with every reference; the best match names the wrestler and attire, and
its difference is the verdict (MATCH under 4, CLOSE under 10, else DIFFERS; the margin to the next-best other wrestler shows how sure the
naming is). The sheet puts each packed atlas next to its best reference. Pictures are game-derived: under out\\ only.
"""
import argparse
import glob
import json
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SIZE = (256, 128)


def small(path):
    return np.asarray(Image.open(path).convert('RGB').resize(SIZE, Image.BILINEAR)).astype(np.float32)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--packs', default=os.path.join(REPO, 'out', 'captures', 'live'))
    ap.add_argument('--refs', default=os.path.join(REPO, 'out', 's100', 'attires'))
    ap.add_argument('--sheet', default=os.path.join(REPO, 'out', 's100', 'attire_check.png'))
    a = ap.parse_args()
    names = {e['id']: e['name'] for e in json.load(open(os.path.join(REPO, 'db', 'ids', 'characters.json'), encoding='utf-8'))['entries']}
    refs = {}
    for p in glob.glob(os.path.join(a.refs, 'attire_*_*.png')):
        m = re.match(r'attire_(\d+)_(\d+)\.png$', os.path.basename(p))
        if m:
            refs[(int(m.group(1)), int(m.group(2)))] = (p, small(p))
    packs = sorted(glob.glob(os.path.join(a.packs, 'pack_*.png')))
    if not packs or not refs:
        print('nothing to compare: %d packed, %d references' % (len(packs), len(refs)))
        return 1
    rows = []
    for p in packs:
        img = small(p)
        scores = sorted((float(np.abs(img - r[1]).mean()), key) for key, r in refs.items())
        best_d, best = scores[0]
        other = next((d for d, k in scores[1:] if k[0] != best[0]), float('nan'))
        verdict = 'MATCH' if best_d < 4 else ('CLOSE' if best_d < 10 else 'DIFFERS')
        rows.append((p, best, best_d, other, verdict))
        print('%-36s %-8s %3d %-28s attire %d  difference %.2f (next wrestler %.2f)' % (
            os.path.basename(p), verdict, best[0], names.get(best[0], '?')[:28], best[1], best_d, other))
    tw, th = SIZE
    sheet = Image.new('RGB', (2 * tw + 10, len(rows) * (th + 16)), (30, 30, 30))
    d = ImageDraw.Draw(sheet)
    for i, (p, best, best_d, other, verdict) in enumerate(rows):
        y = i * (th + 16)
        d.text((3, y + 2), '%s: ours (left) vs files (right) %d %s a%d, %.2f %s' % (os.path.basename(p)[:22], best[0], names.get(best[0], '?')[:18],
                                                                                     best[1], best_d, verdict), fill=(255, 220, 0))
        sheet.paste(Image.open(p).convert('RGB').resize(SIZE, Image.BILINEAR), (0, y + 16))
        sheet.paste(Image.open(refs[best][0]).convert('RGB').resize(SIZE, Image.BILINEAR), (tw + 10, y + 16))
    sheet.save(a.sheet)
    print('sheet: %s' % a.sheet)
    return 0


if __name__ == '__main__':
    sys.exit(main())
