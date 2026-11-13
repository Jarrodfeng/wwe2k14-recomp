"""Lane 135: the geometry of the small passes' draws: is it constant across draws and frames? Numbers + a few rows. Usage: small_geom.py CAPTURE FRAME[,FRAME...] [dump]"""
import sys, os, hashlib, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
import shadow_census as sc
import numpy as np
SITES = {0x8279A19C: 'paste_a', 0x82799CB4: 'paste_b', 0x82799DC8: 'paste_c', 0x82C20B90: 'prelight'}


def main():
    cap = sc.Cap(sys.argv[1])
    frames = [int(x) for x in sys.argv[2].split(',')]
    dump = len(sys.argv) > 3
    seen = collections.defaultdict(set)
    for fi in frames:
        f = cap.frame(fi)
        d = f.draws
        o = f.origins
        cs = o['call_site'].astype(np.int64)
        for i in range(len(d)):
            if int(cs[i]) not in SITES:
                continue
            dr = d[i]
            stride, els = sc.layout_of(f, dr)
            vb = cap.geo[int(dr['vertex_offset']):int(dr['vertex_offset']) + int(dr['vertex_bytes'])].tobytes()
            ib = cap.geo[int(dr['index_offset']):int(dr['index_offset']) + int(dr['index_bytes'])].tobytes()
            hv, hi = hashlib.md5(vb).hexdigest()[:8], hashlib.md5(ib).hexdigest()[:8]
            seen[SITES[int(cs[i])]].add((hv, hi, int(dr['vertex_count']), int(dr['index_count'])))
            if dump and fi == frames[0]:
                rows = np.frombuffer(vb, np.uint8).reshape(-1, stride)
                fl = rows.copy().view('<f4') if stride % 4 == 0 else None
                print(SITES[int(cs[i])], 'draw', i, 'vertex md5', hv, 'index md5', hi)
                print(' first rows (f32):', fl[:6].tolist())
                print(' last rows:', fl[-3:].tolist())
                idx = np.frombuffer(ib, '>u2' if True else '<u2')
                print(' idx (big-endian u16) first 40:', idx[:40].tolist())
    for k, v in seen.items():
        print(k, 'distinct (vertex md5, index md5, vc, ic):', len(v), sorted(v)[:6])


main()
