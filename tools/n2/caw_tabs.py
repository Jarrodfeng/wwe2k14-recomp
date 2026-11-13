"""Lane 117: the editor's tab strip (4 small T_Sprite quads at y 66-82) - which image each uses, per frame. 0 game runs."""
import sys, os, struct, glob
sys.path.insert(0, os.path.dirname(__file__))
from caw_text_read import frame, get_name, origins, expand_banks
cap = sys.argv[1]
geom = open(os.path.join(cap, 'geometry.bin'), 'rb').read()
fs = sorted(glob.glob(os.path.join(cap, 'frames', 'frame_*.bin')))
prev = None
for n, p in enumerate(fs):
    b, gf, secs, S, runs = frame(p)
    des, dc, do = secs[4]
    tes, tn, to = secs[10]
    lay = [struct.unpack_from('<IIII', b, secs[11][2] + i * secs[11][0]) for i in range(secs[11][1])]
    org = origins(b, secs, S)
    dots = []
    for i in range(dc):
        if org[i][0] != 3: continue
        t = struct.unpack_from('<QQQIIIIIIIIQQIIII', b, do + i * des)
        lo, vo, vc, tf, tc = t[10], t[11], t[15], t[5], t[6]
        if lo == 0xFFFFFFFF or not vc or not tc: continue
        x, y = struct.unpack_from('<2f', geom, vo)
        if 64 <= y <= 68 and 300 < x < 600:
            img = struct.unpack_from('<I', b, to + tf * tes)[0]
            bk = expand_banks(b, secs)[t[7]]; dots.append((round(x), img, tuple(round(v, 2) for v in struct.unpack_from("<4f", bk, 16)), tuple(round(v, 3) for v in struct.unpack_from("<2f", geom, vo + 12 if False else vo + 12)) if False else 0))
    if dots != prev:
        print(n, gf, sorted(dots)); prev = dots
