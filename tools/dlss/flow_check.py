"""Session 24 (DLSS): are the motion vectors DLSS gets right? Compares them with the real image motion between two
consecutive native frames of the same replay.

The real motion: for blocks of the native frame F, block matching against frame F-1 (sum of squared differences over
a search window, sub-pixel by a parabola fit) gives where each block's content was last frame (current -> previous,
the DLSS convention). The vectors: the motion view of a DLSS run (--gpu_dlss_view=5, colour = 0.5 + mv / (2 * scale)
in render pixels, upscaled to the output) at frame F. Prints both per block row band and their difference.

Usage: py tools\\dlss\\flow_check.py --native TAG/ARM --mv TAG/ARM --frame F [--scale 64] [--render-scale 2]
       [--region x0,y0,x1,y1] [--block 48] [--search 48]
"""
import argparse
import glob
import os

import numpy as np
from PIL import Image

ROOT = r"C:\Users\Jarrod\wwe2k14-recomp\out\dlss_s24\frames"


def load(spec, f, mode="L"):
    tag, arm = spec.split("/")
    p = glob.glob(os.path.join(ROOT, tag, arm, f"*_clip_h_f{f}.png"))
    if not p:
        raise SystemExit(f"no frame {f} in {spec}")
    return np.asarray(Image.open(p[0]).convert(mode), np.float64)


def match(cur, prev, x, y, b, s):
    blk = cur[y:y + b, x:x + b]
    best = None
    h, w = prev.shape
    costs = {}
    for dy in range(-s, s + 1, 1):
        for dx in range(-s, s + 1, 1):
            yy, xx = y + dy, x + dx
            if yy < 0 or xx < 0 or yy + b > h or xx + b > w:
                continue
            c = ((prev[yy:yy + b, xx:xx + b] - blk) ** 2).mean()
            costs[(dx, dy)] = c
            if best is None or c < best[0]:
                best = (c, dx, dy)
    if best is None:
        return None
    c0, dx, dy = best

    def sub(cm, c, cp):
        d = cm - 2 * c + cp
        return 0.0 if d <= 1e-9 else 0.5 * (cm - cp) / d
    fx = sub(costs.get((dx - 1, dy), c0), c0, costs.get((dx + 1, dy), c0))
    fy = sub(costs.get((dx, dy - 1), c0), c0, costs.get((dx, dy + 1), c0))
    flat = np.var(blk)
    return dx + fx, dy + fy, c0, flat


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--native", required=True)
    ap.add_argument("--mv", required=True)
    ap.add_argument("--frame", type=int, required=True)
    ap.add_argument("--scale", type=float, default=64.0)
    ap.add_argument("--render-scale", type=float, default=2.0, help="output pixels per render pixel")
    ap.add_argument("--region", default="0,700,2560,1440")
    ap.add_argument("--block", type=int, default=48)
    ap.add_argument("--search", type=int, default=40)
    ap.add_argument("--step", type=int, default=160)
    a = ap.parse_args()
    cur = load(a.native, a.frame)
    prev = load(a.native, a.frame - 1)
    mvimg = load(a.mv, a.frame, "RGB")
    x0, y0, x1, y1 = (int(v) for v in a.region.split(","))
    print(f"frame {a.frame}: block matching vs the motion view (output pixels, current -> previous)")
    print(f"{'x':>5s} {'y':>5s} | {'flow dx':>8s} {'dy':>7s} | {'mv dx':>8s} {'dy':>7s} | {'diff':>6s}  texture")
    diffs = []
    for y in range(y0, y1 - a.block, a.step):
        for x in range(x0, x1 - a.block, a.step):
            r = match(cur, prev, x, y, a.block, a.search)
            if r is None:
                continue
            fdx, fdy, cost, flat = r
            c = mvimg[y + a.block // 2, x + a.block // 2] / 255.0
            mdx = (c[0] - 0.5) * 2 * a.scale * a.render_scale
            mdy = (c[1] - 0.5) * 2 * a.scale * a.render_scale
            d = ((fdx - mdx) ** 2 + (fdy - mdy) ** 2) ** 0.5
            if flat > 20:
                diffs.append(d)
            print(f"{x:5d} {y:5d} | {fdx:8.2f} {fdy:7.2f} | {mdx:8.2f} {mdy:7.2f} | {d:6.2f}  {'flat' if flat <= 20 else ''}")
    if diffs:
        print(f"median |flow - mv| over textured blocks: {np.median(diffs):.2f} px (n={len(diffs)})")


if __name__ == "__main__":
    main()
