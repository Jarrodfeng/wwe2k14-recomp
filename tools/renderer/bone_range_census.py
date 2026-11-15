r"""S102 V1: the crowd card bake's bone ids against the bone palette it can address (offline, 0 game runs).

  py -I tools\renderer\bone_range_census.py CAPTURE [--step N]
The bake program (yAudDefault skinned_lit_lm_vertex) declares WorldMatrixArray[63] (21 bones x 3 rows); the bake body has bone ids as a u8x4 element (ids 0-19 real).
Counts, over the bake draws' distinct vertex blobs, the vertices whose id in each slot (x y z w) is past the palette (id > 20) and the largest |weight| the shader gives that slot
(slots a program reads at its bone count (2 / 3 / 4, from the draw's bool words 1 and 2 like the shader): weights w.x, then 1 - w.x (2 bones) or w.y, then 1 - w.x - w.y (3) or w.z, then the rest (4)). Prints one line; exit 0. Reads the capture in place."""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'crowd'))
import cards_census as cc  # noqa: E402
import pcap  # noqa: E402  (tools\particles, put on the path by cards_census)

PALETTE_BONES = 21


def census(path, step):
    cap = cc.Cap(path)
    seen = set()
    tot = 0
    by_slot = [0, 0, 0, 0]
    max_w = [0.0] * 4
    max_id = 0
    for fi in range(0, len(cap.frames), step):
        f = cap.frame(fi)
        D = f.draws
        if not len(D):
            continue
        veff = cap.effs(D['vs_hash'])
        for d in np.nonzero(veff == cc.EFF_BAKE_VS)[0]:
            r = D[d]
            bl = np.frombuffer(f.buf, '<u4', 40, f.secs[pcap.SEC['bool_loops']][2] + 160 * int(r['bool_loop']))
            count = 2 if (int(bl[0]) >> 1) & 1 else (3 if (int(bl[0]) >> 2) & 1 else 4)
            k = (int(r['vertex_offset']), int(r['vertex_bytes']), int(r['layout']), count)
            if k in seen:
                continue
            seen.add(k)
            count = k[3]
            lay = f.layouts[int(r['layout'])]
            stride = int(lay['stride'])
            vc = int(r['vertex_count'])
            a = np.frombuffer(bytes(cap.geo[k[0]:k[0] + k[1]])[:stride * vc], np.uint8).reshape(vc, stride)
            w = np.frombuffer(np.ascontiguousarray(a[:, 16:28]).tobytes(), '<f4').reshape(vc, 3)
            ids = a[:, 28:32].astype(int)
            if count == 2:
                full = np.stack([w[:, 0], 1 - w[:, 0], 0 * w[:, 0], 0 * w[:, 0]], 1)
            elif count == 3:
                full = np.stack([w[:, 0], w[:, 1], 1 - w[:, 0] - w[:, 1], 0 * w[:, 0]], 1)
            else:
                full = np.concatenate([w, (1 - w.sum(1))[:, None]], 1)
            used = np.arange(4)[None, :] < count          # slots the program reads at this bone count
            tot += vc
            for s in range(4):
                o = (ids[:, s] >= PALETTE_BONES) & used[:, s]
                by_slot[s] += int(o.sum())
                max_id = max(max_id, int(ids[:, s].max()))
                if o.any():
                    max_w[s] = max(max_w[s], float(np.abs(full[o, s]).max()))
    return len(seen), tot, by_slot, max_id, max_w


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--step', type=int, default=20)
    a = ap.parse_args()
    n, tot, by_slot, max_id, max_w = census(a.capture, a.step)
    print('%s: %d bake blobs, %d vertices; ids >= %d per slot x/y/z/w %s; largest id %d; largest |weight| at such a slot x/y/z/w %s' % (
        a.capture, n, tot, PALETTE_BONES, by_slot, max_id, ['%.3f' % v for v in max_w]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
