"""Compare screenshots of one run pairwise (Session 66, F1 frozen-frame checks).

A frozen scene (`cvar wwe2k14_freeze true`) keeps being drawn every frame; if those frames are identical, shots taken
while frozen with a renderer flag off and on compare the same frame exactly, with no replay noise. Prints, per pair,
identical or the share of pixels that differ and the share more than 8 levels apart (any channel).

    py tools\\s66\\shot_diff.py RUN LABEL_A LABEL_B [LABEL_A2 LABEL_B2 ...]
    py tools\\s66\\shot_diff.py RUN --chain fz_a1,fz_a2,fz_a3      (each against the first)
Screenshots: out\\build\\win-amd64-release\\captures\\<RUN>_<LABEL>.png. Exit code = pairs that differ.
"""
import os
import sys

import numpy as np
from PIL import Image

CAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out', 'build', 'win-amd64-release',
                   'captures')


def load(run, label):
    return np.asarray(Image.open(os.path.join(CAP, f'{int(run):03d}_{label}.png')).convert('RGB'), dtype=np.int16)


def compare(run, a, b):
    x, y = load(run, a), load(run, b)
    if x.shape != y.shape:
        print(f'{a} vs {b}: sizes differ {x.shape} {y.shape}')
        return 1
    d = np.abs(x - y).max(axis=2)
    n = d.size
    diff = int((d > 0).sum())
    if not diff:
        print(f'{a} vs {b}: identical')
        return 0
    print(f'{a} vs {b}: {100.0 * diff / n:.3f}% of pixels differ, {100.0 * (d > 8).sum() / n:.3f}% by more than 8 '
          f'levels, max {int(d.max())}')
    return 1


def main():
    args = sys.argv[1:]
    if len(args) < 3:
        sys.exit(__doc__)
    run = args[0]
    bad = 0
    if args[1] == '--chain':
        labels = args[2].split(',')
        for other in labels[1:]:
            bad += compare(run, labels[0], other)
    else:
        for a, b in zip(args[1::2], args[2::2]):
            bad += compare(run, a, b)
    sys.exit(bad)


if __name__ == '__main__':
    main()
