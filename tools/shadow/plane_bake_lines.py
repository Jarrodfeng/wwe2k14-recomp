r"""S102 lane L2: rope LINE geometry, the game's rope shadow against the bake's, measured on the picture (Jarrod's bar: the same lines, same positions, same spacing, same width, only cleaner).
Library + a command line:
  py -I tools\shadow\plane_bake_lines.py OFF_DIR DROPROPES_DIR NOBAKE_DIR X_DIR FRAME [FRAME ...]
Each DIR holds fview --dump-frames pictures (frame_NNNNN.png). The game's rope darkening is gray(dropropes) - gray(off); the bake's is gray(nobake) - gray(X) (nobake = the composite without the bake).
Along ROWS rows of the mat the 1-D darkening profile of each is searched for lines (local maxima over a threshold); each game line is matched to the nearest bake line within MATCH px; for each pair the centre
(sub-pixel centroid over the line) and width (full width at half maximum, sub-pixel) are read. measure() returns the per-pair offsets, the width ratio, the peak ratio and the unmatched counts."""
import sys

import numpy as np
from PIL import Image

ROWS = list(range(540, 712, 8))
MATCH = 8.0


def gray(path):
    return np.asarray(Image.open(path).convert('RGB')).astype(np.float64).mean(2)


def lines(profile, thr):
    """-> [(centre, fwhm, peak)] of the local maxima above thr."""
    out = []
    n = len(profile)
    p = profile
    for i in range(2, n - 2):
        if p[i] >= thr and p[i] >= p[i - 1] and p[i] > p[i + 1] and p[i] >= p[i - 2] and p[i] >= p[i + 2]:
            half = p[i] / 2.0
            l = i
            while l > 0 and p[l] > half:
                l -= 1
            r = i
            while r < n - 1 and p[r] > half:
                r += 1
            if p[l] >= half or p[r] >= half or r - l > 60:
                continue
            xl = l + (half - p[l]) / (p[l + 1] - p[l]) if p[l + 1] != p[l] else l
            xr = r - 1 + (p[r - 1] - half) / (p[r - 1] - p[r]) if p[r - 1] != p[r] else r
            seg = np.arange(l, r + 1)
            w = np.clip(p[l:r + 1], 0, None)
            c = float((seg * w).sum() / w.sum()) if w.sum() > 0 else float(i)
            out.append((c, float(xr - xl), float(p[i])))
    return out


def measure(off, dr, nb, x, rows=ROWS, thr=1.5):
    dg = gray(dr) - gray(off)
    db = gray(nb) - gray(x)
    pairs = []
    miss_g = miss_b = 0
    for y in rows:
        lg = lines(np.convolve(dg[y], np.ones(3) / 3, 'same'), thr)
        lb = lines(np.convolve(db[y], np.ones(3) / 3, 'same'), thr)
        used = set()
        for c, w, pk in lg:
            best = min(range(len(lb)), key=lambda k: abs(lb[k][0] - c), default=None)
            if best is not None and abs(lb[best][0] - c) <= MATCH and best not in used:
                used.add(best)
                pairs.append((lb[best][0] - c, lb[best][1] / w if w else 0.0, lb[best][2] / pk if pk else 0.0, w, lb[best][1]))
            else:
                miss_g += 1
        miss_b += len(lb) - len(used)
    return dict(pairs=pairs, miss_game=miss_g, miss_bake=miss_b)


def summary(m):
    p = np.array(m['pairs']) if m['pairs'] else np.zeros((0, 5))
    if not len(p):
        return dict(n=0, off_med=0, off_max=0, width_ratio=0, peak_ratio=0, miss_game=m['miss_game'], miss_bake=m['miss_bake'], w_game=0, w_bake=0)
    return dict(n=len(p), off_med=float(np.median(np.abs(p[:, 0]))), off_max=float(np.abs(p[:, 0]).max()), width_ratio=float(np.median(p[:, 1])), peak_ratio=float(np.median(p[:, 2])),
                miss_game=m['miss_game'], miss_bake=m['miss_bake'], w_game=float(np.median(p[:, 3])), w_bake=float(np.median(p[:, 4])))


if __name__ == '__main__':
    off, dr, nb, x = sys.argv[1:5]
    tot = []
    for f in sys.argv[5:]:
        name = 'frame_%05d.png' % int(f)
        m = measure(*[d + '/' + name for d in (off, dr, nb, x)])
        s = summary(m)
        print(f, {k: round(v, 3) for k, v in s.items()})
