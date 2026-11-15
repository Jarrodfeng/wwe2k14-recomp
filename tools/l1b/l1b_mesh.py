r"""S102 lane L1b: the pre-light mesh and clip transform, read from the captures alone (0 game runs). Evidence for shaders\rdr_light_cubes.comp and src\renderer_adapter\prelight_compute.h.
  py -I tools\l1b\l1b_mesh.py CAPTURE_DIR [FRAME ...]
For every pre-light draw (call site 0x82C20B90) of each listed frame (default: every 10th): the draw has 726 vertices of stride 24 (position xyz, direction xyz) and 1380 u16 indices; the vertices are six
11 x 11 grids (x = 16 f + 1.6 c, y = -1.6 r, the normalised bilinear point of the face's four corner vectors) and the strips are (r+1,c) (r,c) (r+1,c+1) (r,c+1) with a restart after each: every cell is split
along its top-left / bottom-right diagonal. Also prints the clip transform (ScreenScale, ndc scale / offset, viewport) and the texel -> grid mapping the planner derives from it (expected 1/6.4, 0, 1/6.4, 0).
Exit 1 when any draw differs from the model or a capture cannot be read."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
sys.path.insert(0, os.path.join(HERE, '..', 'solver'))
import numpy as np  # noqa: E402
import shadow_census as sc  # noqa: E402
import capread  # noqa: E402

PRE = 0x82C20B90
CORNERS = np.array([[1, 1, 1], [1, 1, -1], [1, -1, 1], [1, -1, -1], [-1, 1, -1], [-1, 1, 1], [-1, -1, -1], [-1, -1, 1], [-1, 1, -1], [1, 1, -1], [-1, 1, 1], [1, 1, 1],
                    [-1, -1, 1], [1, -1, 1], [-1, -1, -1], [1, -1, -1], [-1, 1, 1], [1, 1, 1], [-1, -1, 1], [1, -1, 1], [1, 1, -1], [-1, 1, -1], [1, -1, -1], [-1, -1, -1]], dtype=np.float64).reshape(6, 4, 3)


def model():
    verts = np.zeros((726, 6))
    for f in range(6):
        tl, tr, bl, br = CORNERS[f]
        for r in range(11):
            for c in range(11):
                u, w = c * 0.1, r * 0.1
                v = (tl * (1 - u) + tr * u) * (1 - w) + (bl * (1 - u) + br * u) * w
                v = v / np.linalg.norm(v)
                verts[f * 121 + r * 11 + c] = [16 * f + 1.6 * c, -1.6 * r, 0, *v]
    idx = []
    for f in range(6):
        for k in range(10):
            for j in range(11):
                idx += [f * 121 + (k + 1) * 11 + j, f * 121 + k * 11 + j]
            idx.append(0xFFFF)
    return verts, np.array(idx, dtype=np.uint16)


def main():
    if len(sys.argv) < 2:
        print('FAIL usage: l1b_mesh.py CAPTURE_DIR [FRAME ...]')
        return 1
    cap = sc.Cap(sys.argv[1])
    frames = [int(x) for x in sys.argv[2:]] or list(range(0, len(cap.frames), 10))
    mv, mi = model()
    bad = draws = 0
    worst_v = 0.0
    maps = set()
    for fi in frames:
        f = cap.frame(fi)
        d = f.draws
        cs = sc.pcap.queue_site(f.origins)
        pre = [i for i in range(len(d)) if int(cs[i]) == PRE]
        es, cnt, off = f.secs[capread.SEC['dynamics']]
        dyn = np.frombuffer(f.buf, np.uint8, es * cnt, off).reshape(cnt, es)
        for i in pre:
            draws += 1
            ok = int(d[i]['vertex_count']) == 726 and int(d[i]['index_count']) == 1380 and int(d[i]['vertex_bytes']) == 17424 and int(d[i]['index_bytes']) == 2760
            if ok:
                stride, _ = sc.layout_of(f, d[i])
                rows = sc.vertex_rows(cap, d[i], stride)
                v = np.ascontiguousarray(rows).view('<f4').reshape(726, -1)[:, :6].astype(np.float64)
                err = float(np.abs(v - mv).max())
                worst_v = max(worst_v, err)
                idx = np.frombuffer(cap.geo[int(d[i]['index_offset']):int(d[i]['index_offset']) + 2760].tobytes(), '<u2')
                ok = err < 2e-5 and np.array_equal(idx, mi)
            vb = f.bank(int(d[i]['vs_bank'])).view('<f4').reshape(-1, 4)[201].astype(np.float64)
            fl = dyn[int(d[i]['dynamic'])].copy().view('<f4')
            ns, no = fl[34:37].astype(np.float64), fl[37:40].astype(np.float64)
            sx, sy, ox, oy = vb
            xs = (2 / 384) / (ns[0] * sx * 1.6)
            xo = ((0.5 * (2 / 384) - 1 - no[0]) / ns[0] - ox) / (sx * 1.6)
            ys = -(2 / 64) / (ns[1] * sy * 1.6)
            yo = -(((0.5 * (2 / 64) - 1 - no[1]) / ns[1] - oy) / (sy * 1.6))
            maps.add(tuple(np.round([xs, xo, ys, yo], 6)))
            if not ok:
                bad += 1
                print('FAIL frame %d draw %d: the mesh is not the modelled one' % (fi, i))
    print('%d pre-light draws over %d frames: mesh equals the model (worst vertex error %.2e), bad %d; texel -> grid mappings seen: %s (expected (0.15625, 0, 0.15625, 0))' % (draws, len(frames), worst_v, bad, sorted(maps)))
    return 1 if bad or not draws else 0


if __name__ == '__main__':
    sys.exit(main())
