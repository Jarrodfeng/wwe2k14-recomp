"""Lane 117: for one frame, per run string: draw count, first glyph quad vertex floats + layout elements, ps/vs bank colour words. 0 game runs."""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(__file__))
from caw_text_read import frame, get_name

cap = sys.argv[1]; fr = sys.argv[2]
b, gf, secs, S, runs = frame(os.path.join(cap, 'frames', 'frame_%06d.bin' % int(fr)))
geom = open(os.path.join(cap, 'geometry.bin'), 'rb').read()
def sec(i):
    return secs[i]
des, dc, do = sec(4)
lay = [struct.unpack_from('<IIII', b, sec(11)[2] + i * sec(11)[0]) for i in range(sec(11)[1])]
els = [struct.unpack_from('<HHBBBBi', b, sec(12)[2] + i * sec(12)[0]) for i in range(sec(12)[1])]
bank_es, bank_n, bank_o = sec(13)
orig_es, orig_n, orig_o = sec(64)
first = {}
for d, s, u, f in runs:
    first.setdefault(get_name(S, s), []).append(d)
def draw(i):
    o = do + i * des
    (vh, ph, vph, fx, dy, tf, tc, vb, pb, bl, lo, vo, io, vbytes, ib, vc, ic) = struct.unpack_from('<QQQIIIIIIIIQQIIII', b, o)
    return dict(vs=vh, ps=ph, tf=tf, tc=tc, vb=vb, pb=pb, lay=lo, vo=vo, vbytes=vbytes, vc=vc, ic=ic, fx=fx, dy=dy)
show = int(sys.argv[3]) if len(sys.argv) > 3 else 1
for name, ds in first.items():
    d = draw(ds[0])
    print(repr(name), len(ds), 'draw', ds[0], 'vs', hex(d['vs']), 'ps', hex(d['ps']), 'vcount', d['vc'], 'bytes', d['vbytes'], 'bank ps', d['pb'], 'fx', d['fx'], 'dy', d['dy'])
    if d['lay'] != 0xFFFFFFFF:
        L = lay[d['lay']]
        e = els[L[0]:L[0] + L[1]]
        print('   layout stride', L[2], [(x[2], x[3], x[0], x[1]) for x in e])
        stride = L[2]
        v = geom[d['vo']:d['vo'] + d['vbytes']]
        for k in range(min(show, d['vc'])):
            print('   v%d' % k, struct.unpack_from('<%df' % (stride // 4), v, k * stride))
