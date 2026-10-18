"""Session 16 item 7: which vertex shader draws what. Reads a matte_classify run (tools/matte/make_classify_route.py):
captures/NNN_cf_base_* and NNN_cf_KK_* matte dumps, each KK taken with only shader KK skipped in the post-match freeze
(the same frame redrawn). For each KK: the pixels where the base depth is nearer than KK's (what KK drew), coverage,
and a contact sheet of the masks over the base colour. Output under out/harness/matte/ (game-derived, local).

    py tools/matte/classify.py RUN [--route tools/input_scripts/short/matte_classify.txt]
"""
import argparse
import json
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import depth as D  # noqa: E402
import mask as M  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CAP = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'captures')
OUT = os.path.join(REPO, 'out', 'harness', 'matte')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run', type=int)
    ap.add_argument('--route', default=os.path.join(REPO, 'tools', 'input_scripts', 'short', 'matte_classify.txt'))
    a = ap.parse_args()
    hashes = re.findall(r'^cvar gpu_debug_skip_vertex_shader ([0-9A-F]{16})$', open(a.route).read(), re.M)
    base = os.path.join(CAP, '%03d_cf_base' % a.run)
    db, cb = M.frame_dumps(base)
    zb = M.linear(base, db)
    px, _ = D.decode_color(cb)
    rgb = px[..., [1, 2, 3]].astype(np.float32)
    os.makedirs(OUT, exist_ok=True)
    tiles, rec = [], []
    for i, h in enumerate(hashes):
        pre = os.path.join(CAP, '%03d_cf_%02d' % (a.run, i))
        try:
            dk, _ = M.frame_dumps(pre)
        except SystemExit:
            continue
        zk = M.linear(pre, dk)
        m = (zk - zb) > 2.0
        ys, xs = np.nonzero(m)
        box = [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())] if len(xs) else None
        rec.append(dict(index=i, hash=h, coverage=round(float(m.mean()), 5), box=box))
        t = rgb.copy()
        t[m] = 0.35 * rgb[m] + 0.65 * np.array([255, 0, 255], np.float32)
        im = Image.fromarray(t.astype(np.uint8)).resize((427, 240))
        ImageDraw.Draw(im).text((4, 4), '%02d %s %.2f%%' % (i, h[:8], 100 * m.mean()), fill='yellow')
        tiles.append(im)
    cols = 4
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new('RGB', (427 * cols, 240 * rows))
    for i, im in enumerate(tiles):
        sheet.paste(im, ((i % cols) * 427, (i // cols) * 240))
    sp = os.path.join(OUT, 'classify_%d.png' % a.run)
    sheet.save(sp)
    with open(os.path.join(OUT, 'classify_%d.json' % a.run), 'w') as f:
        json.dump(rec, f, indent=1)
    for r in rec:
        print('%02d %s coverage %6.2f%% box %s' % (r['index'], r['hash'], 100 * r['coverage'], r['box']))
    print(sp)
    return 0


if __name__ == '__main__':
    sys.exit(main())
