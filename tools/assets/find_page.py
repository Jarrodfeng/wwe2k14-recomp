"""Session 97: find a 512x512 32-bit tiled page in a full physical-memory dump by content: at every 4 KB-aligned base,
untile a 32x32 sample grid (tiled_offsets of untile_page.py) and correlate each byte lane with the file lightmap
(fade_lightmap00 of the arena) at the same texels. Prints the best bases. Local dumps only (never committed).

  py -I tools\\assets\\find_page.py DUMP_DIR [--arena 5] [--top 10]
DUMP_DIR holds phys_0.bin .. phys_7.bin (64 MB each, physical 0x00000000 upward).
"""
import argparse
import glob
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'wm30'))
import untile_page as up  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--arena', type=int, default=5)
    ap.add_argument('--top', type=int, default=10)
    ap.add_argument('--game', default=os.path.join(HERE, '..', '..', 'game_data', 'extracted'))
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.dir, 'phys_*.bin')), key=lambda p: int(p.rsplit('_', 1)[1].split('.')[0]))
    mem = np.concatenate([np.fromfile(f, np.uint8) for f in files])
    ref = up.file_lightmap(a.game, a.arena)                     # (512, 512, 3) floats 0..1
    ref_l = ref[..., :3].mean(-1) if ref.ndim == 3 else ref
    step = 16
    ys, xs = np.mgrid[0:512:step, 0:512:step]
    off_all = up.tiled_offsets(512, 512, 2)                    # bytes of each texel (4 bytes per texel)
    offs = off_all[ys, xs].ravel()                              # sample offsets
    refv = ref_l[ys, xs].ravel().astype(np.float64)
    refv = (refv - refv.mean()) / (refv.std() + 1e-9)
    span = int(off_all.max()) + 4
    bases = np.arange(0, len(mem) - span, 4096, dtype=np.int64)
    best = []
    chunk = 2048
    for i in range(0, len(bases), chunk):
        b = bases[i:i + chunk]
        for lane in range(4):
            v = mem[b[:, None] + offs[None, :] + lane].astype(np.float64)
            sd = v.std(1)
            ok = sd > 2
            v = (v - v.mean(1, keepdims=True)) / (sd[:, None] + 1e-9)
            c = (v @ refv) / len(refv)
            c[~ok] = 0
            for j in np.argsort(-c)[:3]:
                best.append((c[j], int(b[j]), lane))
    best.sort(reverse=True)
    for c, base, lane in best[:a.top]:
        print('phys 0x%08X lane %d correlation %.3f' % (base, lane, c))
    return 0


if __name__ == '__main__':
    sys.exit(main())
