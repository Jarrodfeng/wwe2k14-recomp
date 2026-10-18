"""DLAA step 2 check (Session 18): camera motion vectors vs the measured image motion.

  py tools/dlaa/mv_check.py RUN [--scale 16]

RUN must come from tools/input_scripts/short/dlaa_mv_check.txt with --gpu_dlaa_color_base set to the frame's final
colour resolve (the debug colours then reach the screenshot unchanged). It reads:
  mv_still / mv_pan / mv_pan2: the motion debug view (colour = 0.5 + motion / (2 x scale), red = x, green = y);
  pan_a / pan_b: two consecutive plain frames of the same pan; their measured shift (phase correlation on tiles,
  tools/dlaa/jitter_check.py) is the real per-frame motion (b relative to a).
The vectors point from this frame to the previous one, so they should equal minus the a -> b shift (same pan speed;
the pan runs on CineCam's smoothed frame time, so consecutive frames move by nearly the same amount).
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
from jitter_check import CAP, grey, shift  # noqa: E402


def decode(path, scale):
    a = np.asarray(Image.open(path).convert('RGB'), np.float64) / 255.0
    return (a[..., 0] - 0.5) * 2 * scale, (a[..., 1] - 0.5) * 2 * scale


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--scale', type=float, default=16)
    a = ap.parse_args()
    for lab in ('mv_still', 'mv_pan', 'mv_pan2'):
        p = os.path.join(CAP, f'{a.run}_{lab}.png')
        if not os.path.exists(p):
            continue
        mx, my = decode(p, a.scale)
        h, w = mx.shape
        cols = [f'{np.median(mx[:, int(w * f) - 5:int(w * f) + 5]):+.2f}' for f in (0.1, 0.3, 0.5, 0.7, 0.9)]
        print(f'{lab}: motion x median {np.median(mx):+.3f} px (columns 10/30/50/70/90 %: {" ".join(cols)}), '
              f'y median {np.median(my):+.3f} px')
    pa, pb = (os.path.join(CAP, f'{a.run}_{n}.png') for n in ('pan_a', 'pan_b'))
    if os.path.exists(pa) and os.path.exists(pb):
        ga, gb = grey(pa), grey(pb)
        t, xs, ys = 256, [], []
        for y0 in range(0, ga.shape[0] - t + 1, t):
            for x0 in range(0, ga.shape[1] - t + 1, t):
                ra, rb = ga[y0:y0 + t, x0:x0 + t], gb[y0:y0 + t, x0:x0 + t]
                if ra.std() < 4:
                    continue
                sx, sy, peak = shift(ra, rb)
                if peak > 0.2 and abs(sx) < 40 and abs(sy) < 40:
                    xs.append(sx), ys.append(sy)
        if xs:
            print(f'pan_a -> pan_b measured shift: x {np.median(xs):+.3f} y {np.median(ys):+.3f} px over {len(xs)} '
                  f'tiles (so the vectors should read about x {-np.median(xs):+.3f})')


if __name__ == '__main__':
    main()
