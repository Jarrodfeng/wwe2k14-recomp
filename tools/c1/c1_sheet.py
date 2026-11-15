r"""S102 lane C1: the sheet of the five pre-light cubes' faces, switch off (strip + copies) against on (one layered pass), every face of every cube, Bryan's entrance (0 game runs: fview headless).
  py -I tools\c1\c1_sheet.py [--frame 60] [--out DIR] [--cubes 245,1284,1285,1286,1287] [--root DIR]
One PNG: left block = off, right block = on, one row per cube, six columns (layers 0..5, 2x), and a third block with the absolute difference x 8 (black = byte-equal). Prints the max difference per cube.
Exit 1 when a face cannot be read or any difference is non-zero."""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--frame', type=int, default=60)
    ap.add_argument('--cubes', default='245,1284,1285,1286,1287')
    ap.add_argument('--out', default='')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_dir = main_checkout(root)
    fview = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    cap = os.path.join(main_dir, 'out', 'lane86', 'bryan', 'capture')
    out_dir = a.out or os.path.join(main_dir, 'out', 's102', 'sheets', 'c1')
    work = os.path.join(root, 'out', 'c1', 'sheet')
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    cubes = [int(c) for c in a.cubes.split(',')]

    def face(arm, extra, cube, layer):
        path = os.path.join(work, '%s_%d_%d.png' % (arm, cube, layer))
        if os.path.exists(path):
            os.remove(path)
        env = dict(os.environ, FVIEW_IMAGE_LAYER=str(layer))
        subprocess.run([fview, '--capture', cap, '--frame', str(a.frame), '--image-png', '%d,%s' % (cube, path), '--small-passes-ours'] + extra, capture_output=True, text=True, env=env, timeout=600)
        if not os.path.exists(path):
            print('FAIL  cannot read cube %d layer %d (%s)' % (cube, layer, arm))
            return None
        return np.array(Image.open(path).convert('RGB')).astype(int)
    tile = 128
    sheet = Image.new('RGB', (3 * (6 * tile + 20) + 40, len(cubes) * (tile + 8) + 60), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    bad = 0
    for blk, title in enumerate(('OFF: strip + 30 copies', 'ON: one layered pass, no strip, no copies', 'difference x 8 (black = byte-equal)')):
        draw.text((10 + blk * (6 * tile + 20), 4), title, fill=(230, 230, 230))
    for row, cube in enumerate(cubes):
        worst = 0
        for layer in range(6):
            off = face('off', [], cube, layer)
            on = face('on', ['--prelight-layered'], cube, layer)
            if off is None or on is None:
                bad += 1
                continue
            diff = np.abs(off - on)
            worst = max(worst, int(diff.max()))
            for blk, img in enumerate((off, on, np.clip(diff * 8, 0, 255))):
                tile_img = Image.fromarray(img.astype(np.uint8)).resize((tile, tile), Image.NEAREST)
                sheet.paste(tile_img, (10 + blk * (6 * tile + 20) + layer * tile, 24 + row * (tile + 8)))
        draw.text((2, 24 + row * (tile + 8)), str(cube), fill=(255, 220, 120))
        print('cube %d: max difference over six faces %d' % (cube, worst))
        bad += 1 if worst else 0
    path = os.path.join(out_dir, 'c1_prelight_faces_off_vs_on_frame%d.png' % a.frame)
    sheet.save(path)
    print('sheet: %s' % path)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
