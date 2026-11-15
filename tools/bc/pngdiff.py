#!/usr/bin/env python3
"""S102 lane U1: compare two shot folders frame by frame: py -I tools\\bc\\pngdiff.py DIR_A DIR_B -> per frame max channel difference, pixels over 1 / 8 levels, mean luma of each."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.int16)


def main():
    a, b = Path(sys.argv[1]), Path(sys.argv[2])
    names = sorted(p.name for p in a.glob('frame_*.png'))
    if not names:
        print('FAIL no frames in', a)
        return 1
    for n in names:
        if not (b / n).exists():
            print(f'{n}: missing in {b}')
            continue
        x, y = load(a / n), load(b / n)
        d = np.abs(x - y).max(axis=2)
        print(f'{n}: max {int(d.max())}, over 1: {int((d > 1).sum())}, over 8: {int((d > 8).sum())} of {d.size}; mean {x.mean():.2f} vs {y.mean():.2f}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
