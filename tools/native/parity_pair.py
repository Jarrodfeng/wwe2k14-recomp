"""Picture parity between two runs of the same screenshot route (Session 45 prep, for the native-frame renderer).

Pairs the screenshots of two runs by name (captures\\RUN_NAME.png), and per pair prints the share of pixels in each
difference band (largest RGB channel difference: 0, 1-2, 3-8, 9-32, 33-255 levels) and writes a difference picture
(black = equal, grey = small, red = more than 8 levels) to out\\native\\parity_A_B\\NAME.png. The last column is the
share of pixels more than 8 levels apart, the number to watch: replay frames are near-identical, not byte-identical,
so compare it against a reference pair of the same settings (two runs of the reference) before calling anything a
defect. Output under out\\ only (gitignored: the pictures are the game's).

    py tools\\native\\parity_pair.py RUN_A RUN_B [--captures DIR] [--prefix p_]
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BANDS = ((0, 0), (1, 2), (3, 8), (9, 32), (33, 255))


def shots(captures, run, prefix):
    out = {}
    tag = f'{run}_'
    for f in os.listdir(captures):
        if f.startswith(tag) and f.endswith('.png') and f[len(tag):].startswith(prefix):
            out[f[len(tag):-4]] = os.path.join(captures, f)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_a')
    ap.add_argument('run_b')
    ap.add_argument('--captures', default=os.path.join(R, 'out', 'build', 'win-amd64-release', 'captures'))
    ap.add_argument('--prefix', default='', help='only shots whose name starts with this (e.g. p_)')
    a = ap.parse_args()
    sa, sb = shots(a.captures, a.run_a, a.prefix), shots(a.captures, a.run_b, a.prefix)
    names = sorted(set(sa) & set(sb))
    if not names:
        sys.exit(f'no shots in common between runs {a.run_a} and {a.run_b} in {a.captures}')
    out_dir = os.path.join(R, 'out', 'native', f'parity_{a.run_a}_{a.run_b}')
    os.makedirs(out_dir, exist_ok=True)
    print(f'runs {a.run_a} vs {a.run_b}: {len(names)} shots in common'
          + (f' (only in {a.run_a}: {sorted(set(sa) - set(sb))})' if set(sa) - set(sb) else '')
          + (f' (only in {a.run_b}: {sorted(set(sb) - set(sa))})' if set(sb) - set(sa) else ''))
    print(f'{"shot":<22}' + ''.join(f'{f"{lo}-{hi}":>9}' for lo, hi in BANDS) + f'{">8 lv":>9}')
    worst = 0.0
    for n in names:
        x = np.asarray(Image.open(sa[n]).convert('RGB')).astype(np.int16)
        y_img = Image.open(sb[n]).convert('RGB')
        if y_img.size != (x.shape[1], x.shape[0]):
            y_img = y_img.resize((x.shape[1], x.shape[0]))
        y = np.asarray(y_img).astype(np.int16)
        d = np.abs(x - y).max(axis=2)
        shares = [100.0 * np.count_nonzero((d >= lo) & (d <= hi)) / d.size for lo, hi in BANDS]
        over8 = 100.0 * np.count_nonzero(d > 8) / d.size
        worst = max(worst, over8)
        print(f'{n:<22}' + ''.join(f'{s:8.3f}%' for s in shares) + f'{over8:8.3f}%')
        img = np.zeros(x.shape, np.uint8)
        g = np.clip(d * 8, 0, 255).astype(np.uint8)
        img[..., 0] = img[..., 1] = img[..., 2] = g
        img[d > 8] = (255, 0, 0)
        Image.fromarray(img).save(os.path.join(out_dir, f'{n}.png'))
    print(f'worst share more than 8 levels apart: {worst:.3f}%; difference pictures in {out_dir}')


if __name__ == '__main__':
    main()
