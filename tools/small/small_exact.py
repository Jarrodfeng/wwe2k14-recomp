"""Lane 135: which float32 recipe reproduces the stream's cube direction bytes exactly? Numbers only (a search tool, not part of the gate)."""
import os
import sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
import numpy as np
import shadow_census as sc

F = np.float32
FACES = [((1, 1, 1), (1, 1, -1), (1, -1, 1)), ((-1, 1, -1), (-1, 1, 1), (-1, -1, -1)), ((-1, 1, -1), (1, 1, -1), (-1, 1, 1)),
         ((-1, -1, 1), (1, -1, 1), (-1, -1, -1)), ((-1, 1, 1), (1, 1, 1), (-1, -1, 1)), ((1, 1, -1), (-1, 1, -1), (1, -1, -1))]
cap = sc.Cap(sys.argv[1])
f = cap.frame(int(sys.argv[2]))
cs = f.origins['call_site'].astype(np.int64)
i = int(np.nonzero(cs == 0x82C20B90)[0][0])
d = f.draws[i]
got = cap.geo[int(d['vertex_offset']):int(d['vertex_offset']) + int(d['vertex_bytes'])].view('<f4').reshape(6, 11, 11, 6)


def score(fn):
    ok = 0
    tot = 0
    for fc, (a, b, c) in enumerate(FACES):
        a, b, c = np.array(a, F), np.array(b, F), np.array(c, F)
        for ii in range(11):
            for jj in range(11):
                v = fn(a, b, c, ii, jj)
                w = got[fc, ii, jj, 3:]
                ok += int((v.view(np.uint32) == w.view(np.uint32)).sum())
                tot += 3
    return ok, tot


def r_div(a, b, c, i, j):
    d = a + (b - a) * F(j / 10.0) + (c - a) * F(i / 10.0)
    return (d / np.sqrt((d * d).sum(dtype=F))).astype(F)


def r_rsq(a, b, c, i, j):
    d = a + (b - a) * F(j / 10.0) + (c - a) * F(i / 10.0)
    return (d * (F(1) / np.sqrt((d * d).sum(dtype=F)))).astype(F)


def r_lerp(a, b, c, i, j):
    u, v = F(j) * F(0.1), F(i) * F(0.1)
    d = a + (b - a) * u + (c - a) * v
    return (d / np.sqrt((d * d).sum(dtype=F))).astype(F)


def r_axes(a, b, c, i, j):
    u, v = F(-1) + F(2) * F(j) / F(10), F(-1) + F(2) * F(i) / F(10)
    d = a * F(0)
    # position on the face: base = a, steps along (b-a)/2 and (c-a)/2 in [0, 2] units
    d = a + (b - a) * F(j) / F(10) + (c - a) * F(i) / F(10)
    return (d / np.sqrt((d * d).sum(dtype=F))).astype(F)


def r_double(a, b, c, i, j):
    d = a.astype(float) + (b - a).astype(float) * (j / 10.0) + (c - a).astype(float) * (i / 10.0)
    return (d / np.sqrt((d * d).sum())).astype(F)


for name, fn in dict(div=r_div, rsq=r_rsq, lerp=r_lerp, axes=r_axes, double=r_double).items():
    print(name, score(fn))
