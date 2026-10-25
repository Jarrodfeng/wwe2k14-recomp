"""Perceptual difference between two runs of the same screenshot route (Session 65 prep, for "looks right" items).

NVIDIA FLIP (flip-evaluator 1.7, BSD-3, installed 2026-10-25) estimates how noticeable the difference between two
pictures is to a viewer, not how many pixels changed: 0 = identical, about 0.01-0.02 = hard to see side by side,
higher = visible (FLIP's own scale; the thresholds below are calibrated against Jarrod's eye where noted).
Pairs the screenshots of two runs by name (captures\\RUN_NAME.png, as tools\\native\\parity_pair.py) and prints, per
shot: mean FLIP, the share of pixels above 0.1 (clearly visible locally), the 99th percentile, and a verdict; writes
FLIP heat maps (magma: dark = same, bright = visible) to the output folder. Output under out\\ only (the pictures are
the game's).

    py tools\\s66\\flip_pair.py RUN_A RUN_B [--prefix p_] [--out DIR] [--ppd 67]

Exit 0 = every shot "likely invisible", 1 = some shot "check by eye" or "visible".
"""
import argparse
import os
import sys

import flip_evaluator as flip
import numpy as np
from PIL import Image

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# Mean FLIP thresholds. Calibration (Jarrod, 2026-10-25 ~23:08): the anisotropic-filter test pack (out\s66\review\
# aniso_test, means 0.014-0.017) "look basically the same" -> invisible below 0.02. The visible bound (0.04) has no
# calibration point yet.
INVISIBLE, VISIBLE = 0.02, 0.04


def shots(captures, run, prefix):
    tag = f'{run}_'
    return {f[len(tag):-4]: os.path.join(captures, f) for f in os.listdir(captures)
            if f.startswith(tag) and f.endswith('.png') and f[len(tag):].startswith(prefix)}


def load(path, size=None):
    img = Image.open(path).convert('RGB')
    if size and img.size != size:
        img = img.resize(size)
    return np.asarray(img).astype(np.float32) / 255.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_a')
    ap.add_argument('run_b')
    ap.add_argument('--captures', default=os.path.join(R, 'out', 'build', 'win-amd64-release', 'captures'))
    ap.add_argument('--prefix', default='p_')
    ap.add_argument('--out', default='')
    ap.add_argument('--ppd', type=float, default=67.0, help='pixels per degree of view (FLIP default 67: a desktop monitor)')
    a = ap.parse_args()
    sa, sb = shots(a.captures, a.run_a, a.prefix), shots(a.captures, a.run_b, a.prefix)
    names = sorted(set(sa) & set(sb))
    if not names:
        sys.exit(f'no shots in common between runs {a.run_a} and {a.run_b} in {a.captures}')
    out = a.out or os.path.join(R, 'out', 's66', 'flip', f'{a.run_a}_{a.run_b}')
    os.makedirs(out, exist_ok=True)
    print(f'FLIP runs {a.run_a} (reference) vs {a.run_b}: {len(names)} shots; thresholds: mean < {INVISIBLE} likely '
          f'invisible, > {VISIBLE} visible')
    print(f'{"shot":<22}{"mean":>9}{"p99":>9}{"> 0.1":>9}  verdict')
    worst = 'likely invisible'
    for n in names:
        ref = load(sa[n])
        test = load(sb[n], (ref.shape[1], ref.shape[0]))
        err, mean, _ = flip.evaluate(ref, test, 'LDR', applyMagma=False, parameters={'ppd': a.ppd})
        err = np.asarray(err).squeeze()
        p99 = float(np.percentile(err, 99))
        over = 100.0 * float(np.count_nonzero(err > 0.1)) / err.size
        verdict = 'likely invisible' if mean < INVISIBLE and over < 0.5 else ('visible' if mean > VISIBLE else 'check by eye')
        if verdict == 'visible' or (verdict == 'check by eye' and worst == 'likely invisible'):
            worst = verdict
        print(f'{n:<22}{mean:>9.4f}{p99:>9.3f}{over:>8.2f}%  {verdict}')
        heat, _, _ = flip.evaluate(ref, test, 'LDR', applyMagma=True, computeMeanError=False, parameters={'ppd': a.ppd})
        Image.fromarray((np.clip(np.asarray(heat), 0, 1) * 255).astype(np.uint8)).save(os.path.join(out, f'{n}_flip.png'))
    print(f'overall: {worst}; heat maps in {out}')
    sys.exit(0 if worst == 'likely invisible' else 1)


if __name__ == '__main__':
    main()
