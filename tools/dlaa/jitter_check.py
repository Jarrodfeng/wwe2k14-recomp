"""DLAA step 1 check (Session 18): measure the sub-pixel shift between jitter screenshots of a still camera.

  py tools/dlaa/jitter_check.py RUN [--ref dj_00] [--others dj_x5,dj_y5,dj_00b,dj_h1,dj_h2] [--tile 256]

For each other screenshot: phase correlation against the reference per tile (grey, Hann window), sub-pixel peak by
a parabola fit; tiles with a weak peak (moving wrestlers, crowd) are dropped and the median of the rest is the shift.
Also reports the fraction of pixels that differ at all (a still camera: only the jitter and moving things).
Screenshots: out/build/win-amd64-release/captures/RUN_LABEL.png (local only).
"""
import argparse
import os

import numpy as np
from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
CAP = os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'captures')


def grey(path):
    return np.asarray(Image.open(path).convert('L'), np.float64)


def sub_peak(c, i, n):
    a, b, d = c[(i - 1) % n], c[i], c[(i + 1) % n]
    den = a - 2 * b + d
    return 0.0 if den == 0 else 0.5 * (a - d) / den


def shift(a, b):
    """Shift of b relative to a (pixels, x right / y down) and the peak strength."""
    win = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    fa, fb = np.fft.fft2((a - a.mean()) * win), np.fft.fft2((b - b.mean()) * win)
    r = fb * np.conj(fa)
    r /= np.abs(r) + 1e-12
    c = np.real(np.fft.ifft2(r))
    y, x = np.unravel_index(np.argmax(c), c.shape)
    dy = sub_peak(c[:, x], y, c.shape[0])
    dx = sub_peak(c[y, :], x, c.shape[1])
    sy = (y + dy) if y < c.shape[0] // 2 else (y + dy - c.shape[0])
    sx = (x + dx) if x < c.shape[1] // 2 else (x + dx - c.shape[1])
    return sx, sy, float(c.max())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--ref', default='dj_00')
    ap.add_argument('--others', default='dj_x5,dj_y5,dj_00b,dj_h1,dj_h2')
    ap.add_argument('--tile', type=int, default=256)
    a = ap.parse_args()
    ref = grey(os.path.join(CAP, f'{a.run}_{a.ref}.png'))
    t = a.tile
    for lab in a.others.split(','):
        p = os.path.join(CAP, f'{a.run}_{lab}.png')
        if not os.path.exists(p):
            print(f'{lab}: missing')
            continue
        img = grey(p)
        xs, ys = [], []
        for y0 in range(0, ref.shape[0] - t + 1, t):
            for x0 in range(0, ref.shape[1] - t + 1, t):
                ra, rb = ref[y0:y0 + t, x0:x0 + t], img[y0:y0 + t, x0:x0 + t]
                if ra.std() < 4:  # flat (black) tile: nothing to lock on
                    continue
                sx, sy, peak = shift(ra, rb)
                if peak > 0.3 and abs(sx) < 3 and abs(sy) < 3:
                    xs.append(sx), ys.append(sy)
        diff = np.abs(img - ref)
        if xs:
            print(f'{lab}: shift x {np.median(xs):+.3f} y {np.median(ys):+.3f} px over {len(xs)} tile(s) '
                  f'(x IQR {np.percentile(xs, 25):+.3f}..{np.percentile(xs, 75):+.3f}); pixels changed '
                  f'{100 * np.mean(diff > 8):.1f} %')
        else:
            print(f'{lab}: no steady tiles; pixels changed {100 * np.mean(diff > 8):.1f} %')


if __name__ == '__main__':
    main()
