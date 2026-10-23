"""Where two same-frame pictures differ (Session 38): the emulated scene and the native one from gpu_native_dump.

Writes a picture of the largest RGB channel difference per pixel (black = equal, brighter = more, red = more than 8
levels) and prints the share of pixels per difference band and the bounding boxes of the largest differing areas
(on a 32-pixel grid). Output to out\\native\\ only.

    py tools\\native\\diff_pair.py EMULATED.bmp NATIVE.bmp OUT.png
"""
import sys

import numpy as np
from PIL import Image


def main():
    a = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(np.int16)
    b = np.asarray(Image.open(sys.argv[2]).convert('RGB')).astype(np.int16)
    if a.shape != b.shape:
        b = np.asarray(Image.open(sys.argv[2]).convert('RGB').resize((a.shape[1], a.shape[0]))).astype(np.int16)
    d = np.abs(a - b).max(axis=2)
    n = d.size
    print(f'{sys.argv[1]} vs {sys.argv[2]}: {a.shape[1]}x{a.shape[0]}')
    for lo, hi in ((0, 0), (1, 2), (3, 8), (9, 32), (33, 255)):
        print(f'  {lo:>3}-{hi:<3} levels: {100.0 * np.count_nonzero((d >= lo) & (d <= hi)) / n:7.3f}%')
    img = np.zeros(a.shape, np.uint8)
    img[..., 0] = np.where(d > 8, 255, np.clip(d * 16, 0, 255))
    img[..., 1] = np.where(d > 8, 0, np.clip(d * 16, 0, 255))
    img[..., 2] = np.where(d > 8, 0, np.clip(d * 16, 0, 255))
    Image.fromarray(img).save(sys.argv[3])
    g = 32
    h, w = d.shape
    cells = (d[: h // g * g, : w // g * g] > 8).reshape(h // g, g, w // g, g).mean(axis=(1, 3))
    top = np.argsort(cells.ravel())[::-1][:8]
    print('  cells (32 px) with the most pixels off by more than 8 levels: (x, y, share)')
    for i in top:
        y, x = divmod(int(i), w // g)
        if cells[y, x] > 0:
            print(f'    ({x * g}, {y * g}) {100 * cells[y, x]:.0f}%')


if __name__ == '__main__':
    main()
