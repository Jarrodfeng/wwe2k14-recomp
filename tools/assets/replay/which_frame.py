r"""Frame replay (Session 98 S5): which probe frame a dumped capture draw belongs to - its vertex constants (camera c56..c59, the bone
palette c76.., the light block c0..c3) against every probe row with the same vertex shader. 0 runs.

  py -I tools\assets\replay\which_frame.py <dumped draw dir>... [--frames 9947,9948]
"""
import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import probe_rows  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('draws', nargs='+')
    ap.add_argument('--frames', default='')
    a = ap.parse_args()
    fr = set(int(x) for x in a.frames.split(',') if x) or None
    for d in a.draws:
        vs = np.frombuffer(open(os.path.join(d, 'cb_vs_1_1.bin'), 'rb').read(), dtype='<f4').reshape(-1, 4).astype(np.float64)
        meta = json.load(open(os.path.join(d, 'meta.json')))
        best = []
        for sw, dr, h, ic, vc, pc in probe_rows.rows(swaps=fr, index_counts={meta['indices']}):
            cam = np.abs(vc[56:60] - vs[56:60]).max()
            pal = np.abs(vc[76:136] - vs[76:136]).max()
            lc = np.abs(vc[0:4] - vs[0:4]).max()
            best.append((pal + cam, sw, dr, h, cam, pal, lc))
        best.sort()
        for b in best[:3]:
            print('%s: frame %d draw %d vs %s  camera diff %.2g palette diff %.2g light diff %.2g' % (os.path.basename(d), *b[1:]))


if __name__ == '__main__':
    main()
