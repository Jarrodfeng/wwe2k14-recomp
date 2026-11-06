"""Session 90: read a draw-probe CSV (--gpu_debug_draw_probe=HASH --gpu_debug_draw_probe_consts=0-255
--gpu_debug_draw_probe_path=CSV) of a skinned vertex shader and summarise its bone palette per draw: c254 (the bone
stride literal the skinned shaders address with), how many 3-register bone matrices from c76 are distinct, and
whether they are all zero / all identical (a uniform palette = a bind pose). Answers "are the bones sane?" for the
T-pose question (docs\\native\\d3d\\S90_TPOSE.md).

  py -I tools\\native\\probe_bones.py out\\s90\\tpose_probe.csv [--first 76] [--bones 20]
"""
import argparse
import csv
import math


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--first', type=int, default=76, help='first bone register (c76)')
    ap.add_argument('--bones', type=int, default=20, help='bone matrices to look at (3 registers each)')
    a = ap.parse_args()
    with open(a.csv, newline='') as f:
        rows = list(csv.DictReader(f))
    if not rows:
        print('no draws in', a.csv)
        return 1
    for r in rows:
        vals = [float(x) for x in r['consts'].split()]
        first = int(r.get('float_first') or 0)
        def c(i):
            k = (i - first) * 4
            return tuple(vals[k:k + 4]) if 0 <= k and k + 4 <= len(vals) else None
        c254 = c(254)
        mats = []
        for b in range(a.bones):
            m = [c(a.first + 3 * b + j) for j in range(3)]
            if None in m:
                break
            mats.append(tuple(x for row in m for x in row))
        distinct = len(set(mats))
        zero = sum(1 for m in mats if all(abs(x) < 1e-12 for x in m))
        bad = sum(1 for m in mats if any(math.isnan(x) or math.isinf(x) for x in m))
        print('swap %s draw %s indices %s: c254 %s; bones %d read, %d distinct, %d all-zero, %d NaN/inf; bone0 %s' % (
            r['swap'], r['draw'], r['index_count'], c254, len(mats), distinct, zero, bad,
            ' '.join('%.3g' % x for x in mats[0]) if mats else '-'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
