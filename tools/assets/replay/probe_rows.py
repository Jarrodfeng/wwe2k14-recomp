"""Frame replay (Session 98 S5): rows of a draw-probe CSV (--gpu_debug_draw_probe_consts=0-511) as numpy banks, for the 0-run checks
(cube_build.py, place_check.py). Reads out\\ only; writes nothing.

  rows(csv, swaps=None, vs=None)  -> [(swap, draw, vs_hash, index_count, vs_c[256,4], ps_c[256,4])]
"""
import numpy as np

PROBE = r'C:\Users\Jarrod\wwe2k14-recomp\out\s98\replay\probe.csv'


def rows(csv=PROBE, swaps=None, vs=None, index_counts=None):
    out = []
    with open(csv) as f:
        next(f)
        for line in f:
            p = line.rstrip('\n').split(',')
            if len(p) < 19:
                continue
            sw = int(p[0])
            if swaps is not None and sw not in swaps:
                continue
            if vs is not None and p[2] not in vs:
                continue
            ic = int(p[4] or 0)
            if index_counts is not None and ic not in index_counts:
                continue
            c = np.array([float(v) for v in p[18].split()], dtype=np.float64)
            if len(c) < 512 * 4:
                continue
            c = c.reshape(-1, 4)
            out.append((sw, int(p[1]), p[2], ic, c[:256], c[256:512]))
    return out


def swaps(csv=PROBE):
    s = []
    with open(csv) as f:
        next(f)
        for line in f:
            sw = int(line.split(',', 1)[0])
            if not s or s[-1] != sw:
                s.append(sw)
    return s
