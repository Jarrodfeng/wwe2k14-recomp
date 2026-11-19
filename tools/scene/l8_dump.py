#!/usr/bin/env python3
r"""S103 L8: dump the stage light-map page pass of a capture frame (the draws that render into the page target, image 1286 in brock_losswin, copied to the page image 388).
  py -I tools\scene\l8_dump.py CAPDIR FRAME [--draws a-b]
Numbers only (draw indices, image ids, provenance file names, constants, vertex floats). Nothing from the game is written."""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'solver'))
import capread as cr  # noqa: E402


def main():
    cap = Path(sys.argv[1])
    fr = int(sys.argv[2])
    lo, hi = (55, 64)
    if '--draws' in sys.argv:
        lo, hi = map(int, sys.argv[sys.argv.index('--draws') + 1].split('-'))
    f = cr.Frame((cap / 'frames' / ('frame_%06d.bin' % fr)).read_bytes())
    geo = np.memmap(cap / 'geometry.bin', dtype='u1', mode='r')
    prov = {int(p['image']): p for p in f.provenance}
    for i in range(lo, hi + 1):
        d = f.draws[i]
        print('draw', i, 'verts', d['vertex_count'], 'idx', d['index_count'], 'vbytes', d['vertex_bytes'], 'vsbank', d['vs_bank'], 'psbank', d['ps_bank'])
        for s in f.slots[d['tex_first']:d['tex_first'] + d['tex_count']]:
            p = prov.get(int(s['image']))
            pn = (f.string(int(p['file_name'])), int(p['source']), int(p['source_image'])) if p is not None else None
            print('   tex slot', int(s['slot']), 'image', int(s['image']), 'prov', pn)
        vb = bytes(geo[int(d['vertex_offset']):int(d['vertex_offset']) + int(d['vertex_bytes'])])
        n = len(vb) // 4
        print('   vertex floats', np.round(np.frombuffer(vb[:n * 4], '<f4'), 4).tolist()[:48])
        for r in range(0, 12):
            v = f.reg(int(d['ps_bank']), r)[0]
            if np.any(v != 0):
                print('   ps c%d' % r, np.round(v, 5).tolist())
        for r in range(0, 8):
            v = f.reg(int(d['vs_bank']), r)[0]
            if np.any(v != 0):
                print('   vs c%d' % r, np.round(v, 5).tolist())


if __name__ == '__main__':
    main()
