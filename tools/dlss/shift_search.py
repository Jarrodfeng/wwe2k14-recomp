"""Session 24 (DLSS): is a test frame shifted against the reference? For each frame, the PSNR (luma) of the test image
resampled by sub-pixel offsets (dx, dy) against the reference; prints the best offset and its gain.

Usage: py tools\\dlss\\shift_search.py TAG TEST_ARM FRAME[,FRAME...] [--ref native2] [--range 1.0] [--step 0.125]
"""
import argparse
import glob
import os
import re

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = r"C:\Users\Jarrod\wwe2k14-recomp\out\dlss_s24\frames"


def load(tag, arm, f):
    p = glob.glob(os.path.join(ROOT, tag, arm, f"*_clip_h_f{f}.png"))
    return np.asarray(Image.open(p[0]).convert("L"), np.float64) if p else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag")
    ap.add_argument("arm")
    ap.add_argument("frames")
    ap.add_argument("--ref", default="native2")
    ap.add_argument("--range", type=float, default=1.0)
    ap.add_argument("--step", type=float, default=0.125)
    a = ap.parse_args()
    for f in [int(x) for x in a.frames.split(",")]:
        r, t = load(a.tag, a.ref, f), load(a.tag, a.arm, f)
        if r is None or t is None:
            print(f, "missing")
            continue
        if t.shape != r.shape:
            t = np.asarray(Image.fromarray(t.astype(np.uint8)).resize(r.shape[::-1], Image.BICUBIC), np.float64)
        m = 16  # ignore the borders
        best = None
        base = None
        offs = np.arange(-a.range, a.range + 1e-9, a.step)
        for dy in offs:
            for dx in offs:
                s = ndimage.shift(t, (dy, dx), order=3, mode="nearest")
                mse = ((s - r)[m:-m, m:-m] ** 2).mean()
                p = 10 * np.log10(255 ** 2 / mse)
                if abs(dx) < 1e-9 and abs(dy) < 1e-9:
                    base = p
                if best is None or p > best[0]:
                    best = (p, dx, dy)
        print(f"frame {f}: PSNR {base:.2f} as is, best {best[0]:.2f} at shift dx {best[1]:+.3f} dy {best[2]:+.3f}")


if __name__ == "__main__":
    main()
