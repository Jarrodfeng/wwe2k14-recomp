"""Lane 135: prove (offline) the closed forms of the two small passes' static meshes against the stream's bytes (numbers only).
  py -I tools\\small\\small_formula.py CAPTURE FRAME
A python copy of the rules, to fit them. The shipped C++ is src\\renderer_scene\\small_passes_core.h; it is compared with the stream's bytes on every draw by the hook itself
(tools\\small\\small_passes_check.py), there is no separate unit test of it."""
import math
import sys
import os
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
import numpy as np
import shadow_census as sc

FACES = [((1, 1, 1), (1, 1, -1), (1, -1, 1)), ((-1, 1, -1), (-1, 1, 1), (-1, -1, -1)), ((-1, 1, -1), (1, 1, -1), (-1, 1, 1)),
         ((-1, -1, 1), (1, -1, 1), (-1, -1, -1)), ((-1, 1, 1), (1, 1, 1), (-1, -1, 1)), ((1, 1, -1), (-1, 1, -1), (1, -1, -1))]


def disc():
    v = np.zeros((11, 17, 6), np.float32)
    for r in range(10):
        s = r / 9.0
        t = math.sin(math.pi * s / 2) ** 2
        R = 0.5 * t
        w = 1.0 - (1.0 - t) ** 5
        for k in range(17):
            a = k * 2 * math.pi / 16
            v[r, k, 0], v[r, k, 1], v[r, k, 5] = 0.5 + R * math.cos(a), 0.5 - R * math.sin(a), w
    sq = [(1, .5)] + [(1, 0)] * 4 + [(0, 0)] * 3 + [(0, 1)] * 4 + [(1, 1)] * 5
    for k in range(17):
        v[10, k, 0], v[10, k, 1] = sq[k]
    return v.reshape(-1, 6)


def cube():
    v = np.zeros((6, 11, 11, 6), np.float32)
    for f, (a, b, c) in enumerate(FACES):
        a, b, c = np.array(a, float), np.array(b, float), np.array(c, float)
        for i in range(11):
            for j in range(11):
                d = a + (b - a) * (j / 10.0) + (c - a) * (i / 10.0)
                d /= np.linalg.norm(d)
                v[f, i, j, :3] = (16 * f + 1.6 * j, -1.6 * i, 0)
                v[f, i, j, 3:] = d
    return v.reshape(-1, 6)


def main():
    cap = sc.Cap(sys.argv[1])
    f = cap.frame(int(sys.argv[2]))
    cs = f.origins['call_site'].astype(np.int64)
    for site, gen, name in ((0x82799CB4, disc, 'disc'), (0x82C20B90, cube, 'cube')):
        i = int(np.nonzero(cs == site)[0][0])
        d = f.draws[i]
        got = cap.geo[int(d['vertex_offset']):int(d['vertex_offset']) + int(d['vertex_bytes'])].view('<f4').reshape(-1, 6)
        want = gen()
        print(name, got.shape, want.shape, 'max abs diff', float(np.abs(got - want).max()) if got.shape == want.shape else 'SHAPE')
        if got.shape == want.shape:
            bad = np.argwhere(np.abs(got - want) > 1e-5)
            print('  worst', bad[:5].tolist())


main()
